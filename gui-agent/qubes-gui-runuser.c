/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2018  Marek Marczykowski-Górecki  <marmarek@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#define HAVE_PAM

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <err.h>
#include <stdbool.h>
#include <qubesdb-client.h>
#include <dbus/dbus.h>

#ifdef HAVE_PAM
#include <security/pam_appl.h>
#endif

pid_t fork_pid = 0;

/* Note: augment_pam_env_with_systemd_env expects out_env_ref to be pointer to
 * a NULL-terminated array of strings consisting of equals-sign-separated
 * key-value pairs. All items in out_env_ref MUST be heap-allocated, as this
 * function is liable to free() any item in the passed-in array in order to
 * replace it with an item obtained from systemd's environment.
 *
 * Note also, this function talks with the systemd user instance, not the
 * system instance (pid 1).
 */
static void augment_pam_env_with_systemd_env(char ***out_env_ref)
{
    DBusConnection *dbus_conn = NULL;
    DBusError error_data = { 0 };
    DBusMessage *env_request = NULL;
    const char *systemd_manager_str = "org.freedesktop.systemd1.Manager";
    const char *environment_str = "Environment";
    dbus_bool_t ret = FALSE;
    DBusMessage *env_reply = NULL;
    int reply_type = 0;
    DBusMessageIter reply_iter = { 0 };
    DBusMessageIter reply_inner_iter = { 0 };
    char *inner_iter_typesig = NULL;
    DBusMessageIter reply_arr_iter = { 0 };
    const char *env_val = NULL;
    char **env_arr = NULL;
    char **out_env_arr = NULL;
    size_t env_arr_len = 0;
    size_t out_env_arr_len = 0;
    size_t out_env_idx = 0;
    size_t env_idx = 0;
    char *out_env_eq_ptr = NULL;
    char *env_eq_ptr = NULL;
    size_t out_env_pre_eq_len = 0;
    size_t env_pre_eq_len = 0;
    bool did_override_env_var = false;
    int current_type = 0;

    if (out_env_ref == NULL)
        errx(1, "augment_pam_env_with_systemd_env: NULL out_env_ref argument is unsupported!\n");
    out_env_arr = *out_env_ref;
    if (out_env_arr == NULL)
        errx(1, "augment_pam_env_with_systemd_env: NULL array in out_env_ref argument is unsupported!\n");
    for (out_env_idx = 0; out_env_arr[out_env_idx] != NULL; out_env_idx++) {
        out_env_arr_len++;
    }
    /* Increment 1 to include the NULL element at the end of the array. */
    out_env_arr_len++;

    /* Initialize D-Bus. */
    dbus_error_init(&error_data);
    dbus_conn = dbus_bus_get(DBUS_BUS_SESSION, &error_data);
    if (dbus_conn == NULL) {
        warnx("augment_pam_env_with_systemd_env: Failed to initialize D-Bus, error name: '%s', error contents: '%s'\n",
              error_data.name,
              error_data.message);
        goto dbus_cleanup;
    }

    /* dbus_bus_get sets up our process to be killed if the D-Bus connection
     * closes. We don't want that, turn that off.
     */
    dbus_connection_set_exit_on_disconnect(dbus_conn, FALSE);

    /* Create a D-Bus method call message for getting the "Environment"
     * property of org.freedesktop.systemd1.Manager.
     */
    env_request = dbus_message_new_method_call("org.freedesktop.systemd1",
                                               "/org/freedesktop/systemd1",
                                               "org.freedesktop.DBus.Properties",
                                               "Get");
    if (env_request == NULL) {
        warnx("augment_pam_env_with_systemd_env: Failed to create D-Bus method call object!\n");
        goto dbus_cleanup;
    }

    ret = dbus_message_append_args(env_request,
                                   DBUS_TYPE_STRING, &systemd_manager_str,
                                   DBUS_TYPE_STRING, &environment_str,
                                   DBUS_TYPE_INVALID);
    if (ret == FALSE) {
        warnx("augment_pam_env_with_systemd_env: Failed to append arguments to D-Bus method call object!\n");
        goto dbus_cleanup;
    }

    /* Send the method call to systemd, waiting a maximum of 500 milliseconds
     * for a response.
     */
    env_reply = dbus_connection_send_with_reply_and_block(dbus_conn,
                                                          env_request,
                                                          500,
                                                          &error_data);
    if (env_reply == NULL) {
        warnx("augment_pam_env_with_systemd_env: Failed to request environment data from systemd, error name: '%s', error contents: '%s'\n",
              error_data.name,
              error_data.message);
        goto dbus_cleanup;
    }

    /* Ensure the reply is a method call return value. */
    reply_type = dbus_message_get_type(env_reply);
    if (reply_type != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        warnx("augment_pam_env_with_systemd_env: Did not get method call return object from systemd!\n");
        goto dbus_cleanup;
    }

    /* D-Bus property get methods return a Variant, which is not a basic type,
     * thus we have to iterate through the method return value to get to the
     * contents.
     */
    ret = dbus_message_iter_init(env_reply, &reply_iter);
    if (ret == FALSE) {
        warnx("augment_pam_env_with_systemd_env: systemd returned an empty method call return object!\n");
        goto dbus_cleanup;
    }

    /* Make sure we actually got a Variant in reply. If so, recurse into it so
     * we can look at its contents.
     */
    if (dbus_message_iter_get_arg_type(&reply_iter) != DBUS_TYPE_VARIANT) {
        warnx("augment_pam_env_with_systemd_env: systemd did not return a variant object!\n");
        goto dbus_cleanup;
    }
    dbus_message_iter_recurse(&reply_iter, &reply_inner_iter);

    /* Ensure the returned Variant contains a string array. The type signature
     * for this data type in D-Bus is "as". If we do have a string array,
     * recurse into it so we can iterate through it.
     */
    inner_iter_typesig = dbus_message_iter_get_signature(&reply_inner_iter);
    if (strcmp(inner_iter_typesig, "as") != 0) {
        warnx("augment_pam_env_with_systemd_env: Variant object from systemd does not contain a string array!\n");
        goto dbus_cleanup;
    }
    if (dbus_message_iter_get_arg_type(&reply_inner_iter)
                != DBUS_TYPE_ARRAY) {
        warnx("augment_pam_env_with_systemd_env: Variant object from systemd reported itself as a string array, but is not an array!\n");
        goto dbus_cleanup;
    }
    dbus_message_iter_recurse(&reply_inner_iter, &reply_arr_iter);

    /* Walk through the elements of the string array, appending them to our
     * internal environment array "env_arr".
     */
    while ((current_type = dbus_message_iter_get_arg_type(&reply_arr_iter))
                != DBUS_TYPE_INVALID) {
        if (current_type != DBUS_TYPE_STRING) {
            warnx("augment_pam_env_with_systemd_env: Non-string item found in string array!\n");
            goto dbus_cleanup;
        }
        dbus_message_iter_get_basic(&reply_arr_iter, &env_val);
        env_arr_len++;
        env_arr = reallocarray(env_arr, env_arr_len, sizeof(char *));
        if (env_arr == NULL)
            err(1, "augment_pam_env_with_systemd_env: Failed to allocate memory for environment array");
        env_arr[env_arr_len - 1] = strdup(env_val);
        if (env_arr[env_arr_len - 1] == NULL)
            err(1, "augment_pam_env_with_systemd_env: Failed to allocate memory for environment item");

        dbus_message_iter_next(&reply_arr_iter);
    }

    /* Merge the environment from systemd with the environment from PAM.
     * Prefer variables from systemd over variables from PAM.
     */
    for (env_idx = 0; env_idx < env_arr_len; env_idx++) {
        env_eq_ptr = strstr(env_arr[env_idx], "=");
        if (env_eq_ptr == NULL)
            errx(1, "augment_pam_env_with_systemd_env: Environment variable without equals sign encountered in systemd environment!\n");
        env_pre_eq_len = env_eq_ptr - env_arr[env_idx];
        did_override_env_var = false;

        for (out_env_idx = 0; out_env_arr[out_env_idx] != NULL;
                    out_env_idx++) {
            out_env_eq_ptr = strstr(out_env_arr[out_env_idx], "=");
            if (out_env_eq_ptr == NULL)
                errx(1, "augment_pam_env_with_systemd_env: Environment variable without equals sign encountered in PAM environment!\n");
            out_env_pre_eq_len = out_env_eq_ptr - out_env_arr[out_env_idx];

            if (out_env_pre_eq_len != env_pre_eq_len)
                continue;

            if (strncmp(out_env_arr[out_env_idx], env_arr[env_idx],
                        env_pre_eq_len) == 0) {
                /* According to `man pam_getenvlist`, "it is the
                 * responsibility of the calling application to free() [the
                 * memory allocated by pam_getenvlist()]". out_env_arr will be
                 * a char ** created by pam_getenvlist(), thus we can safely
                 * free items in out_env_arr.
                 */
                free(out_env_arr[out_env_idx]);

                /* We intentionally are NOT copying the string here. Every
                 * item in env_arr will eventually end up in out_env_arr, so
                 * rather than copying strings from env_arr and then freeing
                 * env_arr, we simply merge all of the pointers from env_arr
                 * into out_env_arr, freeing anything in out_env_arr that is
                 * overridden by something from systemd. This wastes no
                 * memory, and is quite a bit more efficient.
                 */
                out_env_arr[out_env_idx] = env_arr[env_idx];

                /* Signal to the outer loop that we don't need to append an
                 * item to the environment list.
                 */
                did_override_env_var = true;
                break;
            }
        }

        if (!did_override_env_var) {
            /* Append the variable to the list. */
            out_env_arr_len++;
            out_env_arr = reallocarray(out_env_arr, out_env_arr_len,
                                       sizeof(char *));
            if (out_env_arr == NULL)
                err(1, "augment_pam_env_with_systemd_env: Failed to allocate memory for environment item");

            out_env_arr[out_env_arr_len - 1] = NULL;
            /* See above for rationale behind using assignment rather than
             * copying here.
             */
            out_env_arr[out_env_arr_len - 2] = env_arr[env_idx];
        }
    }

dbus_cleanup:
    /* Don't free the elements of env_arr, they have been placed into
     * out_env_arr.
     */
    if (env_arr != NULL)
        free(env_arr);
    if (inner_iter_typesig != NULL)
        dbus_free(inner_iter_typesig);
    if (env_reply != NULL)
        dbus_message_unref(env_reply);
    if (env_request != NULL)
        dbus_message_unref(env_request);
    if (dbus_conn != NULL)
        dbus_connection_unref(dbus_conn);
    *out_env_ref = out_env_arr;
}

#ifdef HAVE_PAM
static int pam_conv_callback(int num_msg, const struct pam_message **msg,
        struct pam_response **resp, void *appdata_ptr __attribute__((__unused__)))
{
    int i;
    struct pam_response *resp_array =
        calloc(num_msg, sizeof(struct pam_response));

    if (resp_array == NULL)
        return PAM_BUF_ERR;

    for (i=0; i<num_msg; i++) {
        if (msg[i]->msg_style == PAM_ERROR_MSG)
            fprintf(stderr, "%s", msg[i]->msg);
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
                msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            resp_array[i].resp = strdup("");
            resp_array[i].resp_retcode = 0;
        }
    }
    *resp = resp_array;
    return PAM_SUCCESS;
}

static struct pam_conv conv = {
    pam_conv_callback,
    NULL
};

/* Start process as given user, register session with PAM (and logind via
 * pam_systemd) first; wait for the process to terminate.
 */
static pid_t do_execute(char *user, char *path, char **argv)
{
    char *tty = NULL;
    struct passwd *pw;
    struct passwd pw_copy;
    int retval=0, status;
    char **env;
    char env_buf[256];
    size_t env_idx;
    pam_handle_t *pamh=NULL;

    if (!user)
        goto error;

    pw = getpwnam (user);
    if (! (pw && pw->pw_name && pw->pw_name[0] && pw->pw_dir && pw->pw_dir[0]
                && pw->pw_passwd)) {
        fprintf(stderr, "user %s does not exist", user);
        goto error;
    }

    /* Make a copy of the password information and point pw at the local
     * copy instead.  Otherwise, some systems (e.g. Linux) would clobber
     * the static data through the getlogin call.
     */
    pw_copy = *pw;
    pw = &pw_copy;
    if ((pw->pw_name = strdup(pw->pw_name)) == NULL ||
        (pw->pw_passwd = strdup(pw->pw_passwd)) == NULL ||
        (pw->pw_dir = strdup(pw->pw_dir)) == NULL ||
        (pw->pw_shell = strdup(pw->pw_shell)) == NULL)
    {
        err(1, "strdup");
    }
    endpwent();

    retval = pam_start("qubes-gui-agent", user, &conv, &pamh);
    if (retval != PAM_SUCCESS)
        goto error;

    /* provide env variables to PAM and the X session */
    retval = snprintf(env_buf, sizeof(env_buf), "HOME=%s", pw->pw_dir);
    if ((unsigned int)retval >= sizeof(env_buf))
        goto error;
    retval = pam_putenv(pamh, env_buf);
    if (retval != PAM_SUCCESS)
        goto error;
    retval = snprintf(env_buf, sizeof(env_buf), "SHELL=%s", pw->pw_shell);
    if ((unsigned int)retval >= sizeof(env_buf))
        goto error;
    retval = pam_putenv(pamh, env_buf);
    if (retval != PAM_SUCCESS)
        goto error;
    retval = snprintf(env_buf, sizeof(env_buf), "USER=%s", pw->pw_name);
    if ((unsigned int)retval >= sizeof(env_buf))
        goto error;
    retval = pam_putenv(pamh, env_buf);
    if (retval != PAM_SUCCESS)
        goto error;
    retval = snprintf(env_buf, sizeof(env_buf), "LOGNAME=%s", pw->pw_name);
    if ((unsigned int)retval >= sizeof(env_buf))
        goto error;
    retval = pam_putenv(pamh, env_buf);
    if (retval != PAM_SUCCESS)
        goto error;
    if (getenv("ENV_PATH"))
        retval = snprintf(env_buf, sizeof(env_buf), "PATH=%s", getenv("ENV_PATH"));
    else if (getenv("PATH"))
        retval = snprintf(env_buf, sizeof(env_buf), "PATH=%s", getenv("PATH"));
    else
        retval = snprintf(env_buf, sizeof(env_buf), "PATH=%s", "/usr/local/bin:/usr/bin:/bin");
    if ((unsigned int)retval >= sizeof(env_buf))
        goto error;
    retval = pam_putenv(pamh, env_buf);
    if (retval != PAM_SUCCESS)
        goto error;

    char *seat = getenv("XDG_SEAT");
    if (seat != NULL) {
        retval = snprintf(env_buf, sizeof(env_buf), "XDG_SEAT=%s", seat);
        if ((unsigned int) retval >= sizeof(env_buf))
            goto error;
        retval = pam_putenv(pamh, env_buf);
        if (retval != PAM_SUCCESS)
            goto error;
    }
    char *session_type = getenv("XDG_SESSION_TYPE");
    if (session_type) {
        retval = snprintf(env_buf, sizeof(env_buf), "XDG_SESSION_TYPE=%s",
                          session_type);
        if ((unsigned int)retval >= sizeof(env_buf))
            goto error;
        retval = pam_putenv(pamh, env_buf);
        if (retval != PAM_SUCCESS)
            goto error;
    }
    retval = pam_putenv(pamh, "XDG_CURRENT_DESKTOP=X-QUBES");
    if (retval != PAM_SUCCESS)
        goto error;
    retval = pam_putenv(pamh, "XDG_SESSION_DESKTOP=X-QUBES");
    if (retval != PAM_SUCCESS)
        goto error;
    char *session_class = getenv("XDG_SESSION_CLASS");
    if (session_class) {
        retval = snprintf(env_buf, sizeof(env_buf), "XDG_SESSION_CLASS=%s",
                          session_class);
        if ((unsigned int)retval >= sizeof(env_buf))
            goto error;
        retval = pam_putenv(pamh, env_buf);
        if (retval != PAM_SUCCESS)
            goto error;
    }
    char *x_display = getenv("DISPLAY");
    if (x_display) {
        retval = pam_set_item(pamh, PAM_XDISPLAY, x_display);
        if (retval != PAM_SUCCESS)
            goto error;
    }
    if (isatty(0) && (tty=ttyname(0))) {
        retval = pam_set_item(pamh, PAM_TTY, tty);
        if (retval != PAM_SUCCESS)
            goto error;
        if (strncmp(tty, "/dev/tty", 8) == 0) {
            retval = snprintf(env_buf, sizeof(env_buf), "XDG_VTNR=%s", tty+8);
            if ((unsigned int)retval >= sizeof(env_buf))
                goto error;
            retval = pam_putenv(pamh, env_buf);
            if (retval != PAM_SUCCESS)
                goto error;
            retval = ioctl(0, VT_ACTIVATE, atoi(tty+8));
            if (retval)
                perror("ioctl(VT_ACTIVATE)");
        }
    }

    /* authenticate */
    retval = pam_authenticate(pamh, 0);
    if (retval != PAM_SUCCESS)
        goto error;

    retval = initgroups(pw->pw_name, pw->pw_gid);
    if (retval == -1) {
        perror("initgroups");
        goto error;
    }

    retval = pam_setcred(pamh, PAM_ESTABLISH_CRED);
    if (retval != PAM_SUCCESS)
        goto error;

    /* open session */
    retval = pam_open_session(pamh, 0);
    if (retval != PAM_SUCCESS)
        goto error;

    fork_pid = fork();

    switch (fork_pid) {
        case -1:
            perror("fork xorg");
            goto error;
        case 0:
            /* child */

            if (setgid (pw->pw_gid))
                exit(126);
            if (setuid (pw->pw_uid))
                exit(126);
            setsid();

            /* This is a copy but don't care to free as we exec later anyway.  */
            env = pam_getenvlist (pamh);

            /* Get the DBUS_SESSION_BUS_ADDRESS from the PAM environment and
             * place it into our own environment, so that we can talk to
             * systemd via D-Bus to get environment variables from it.
             */
            for (env_idx = 0; env[env_idx] != NULL; env_idx++) {
                if (strncmp(env[env_idx], "DBUS_SESSION_BUS_ADDRESS=",
                            strlen("DBUS_SESSION_BUS_ADDRESS=")) == 0) {
                    putenv(strdup(env[env_idx]));
                    break;
                }
            }

            /* Try to augment the environment list from PAM with the
             * environment from the systemd user instance for the current
             * user.
             */
            augment_pam_env_with_systemd_env(&env);

            /* try to enter home dir, but don't abort if it fails */
            retval = chdir(pw->pw_dir);
            if (retval == -1)
                warn("chdir(%s)", pw->pw_dir);

            execve(path, argv, env);
            perror("execve cmd");
            exit(127);
        default:;
    }

    for (;;) {
        pid_t wait_pid = waitpid(fork_pid, &status, 0);
        if (wait_pid == (pid_t)-1) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            goto error;
        }
        if (wait_pid == fork_pid)
            break;
    }

    fork_pid = 0;

    retval = pam_close_session(pamh, 0);
    retval = pam_setcred(pamh, PAM_DELETE_CRED | PAM_SILENT);
    pam_end(pamh, retval);

    if (WIFSIGNALED(status))
        return 128+WTERMSIG(status);
    else
        return WEXITSTATUS(status);
error:
    if (pamh)
        pam_end(pamh, retval);
    return -1;
}

#else
/* in no-PAM case, simply switch to the target user and exec in place - there
 * is no need to keep parent process running as there nothing to cleanup
 */
static pid_t do_execute(char *user, char *path, char **argv)
{
    pid_t pid;
    int retval;
    struct passwd *pw;

    pw = getpwnam (user);
    if (! (pw && pw->pw_name && pw->pw_name[0] && pw->pw_dir && pw->pw_dir[0]
                && pw->pw_passwd)) {
        fprintf(stderr, "user %s does not exist", user);
        goto error;
    }

    if (setenv("HOME", pw->pw_dir, 1))
        goto error;
    if (setenv("SHELL", pw->pw_shell, 1))
        goto error;
    if (setenv("USER", pw->pw_name, 1))
        goto error;
    if (setenv("LOGNAME", pw->pw_name, 1))
        goto error;

    retval = initgroups(pw->pw_name, pw->pw_gid);
    if (retval == -1) {
        perror("initgroups");
        goto error;
    }

    if (setgid (pw->pw_gid))
        exit(126);
    if (setuid (pw->pw_uid))
        exit(126);
    setsid();

    /* try to enter home dir, but don't abort if it fails */
    retval = chdir(pw->pw_dir);
    if (retval == -1)
        warn("chdir(%s)", pw->pw_dir);

    execve(path, argv, env);
    perror("execve cmd");
    return -1;
}
#endif

static void propagate_signal(int signal) {
    if (!fork_pid) {
        exit(0);
    }
    kill(fork_pid, signal);
}

static void usage(char *argv0) {
    fprintf(stderr, "Usage: %s user path arg0 [args ...]\n", argv0);
    fprintf(stderr, "Run a process from *path* with *arg0*, *args*, as user *user*\n");
    fprintf(stderr, "If *user* is the empty string, the user is obtained from /default-user in qubesdb.\n");
#ifdef HAVE_PAM
    fprintf(stderr, "The user session will be registered in pam/logind as graphical one\n");
    fprintf(stderr, "This require the following environment variables to be set:\n");
    fprintf(stderr, " - XDG_SEAT\n");
    fprintf(stderr, " - XDG_SESSION_CLASS\n");
    fprintf(stderr, " - DISPLAY\n");
    fprintf(stderr, "Additionally, stdin needs to be connected to a tty. "
            "XDG_VTNR is set based on it\n");
#endif
}

int main(int argc, char **argv) {
    if (argc < 4) {
        usage(argv[0]);
        exit(1);
    }

    signal(SIGTERM, propagate_signal);
    signal(SIGHUP, propagate_signal);

    char *user = argv[1];
    unsigned int len = 0;
    if (user[0] == 0) {
        qdb_handle_t qdb = qdb_open(NULL);
        if (qdb == NULL)
            err(1, "qdb_open()");
        user = qdb_read(qdb, "/default-user", &len);
        if (user == NULL)
            err(1, "qdb_read(\"/default-user\")");
        if (len == 0)
            errx(1, "username cannot be empty");
    }

    return do_execute(user, argv[2], argv+3);
}
