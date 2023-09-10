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
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <err.h>
#include <qubesdb-client.h>

#ifdef HAVE_PAM
#include <security/pam_appl.h>
#endif

pid_t child_pid = 0;

#ifdef HAVE_PAM
static int pam_conv_callback(int num_msg, const struct pam_message **msg,
        struct pam_response **resp, void *appdata_ptr __attribute__((__unused__)))
{
    int i;
    struct pam_response *resp_array =
        calloc(sizeof(struct pam_response), num_msg);

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
    pam_handle_t *pamh=NULL;
    pid_t pid;

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

    pid = fork();

    switch (pid) {
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

            /* try to enter home dir, but don't abort if it fails */
            retval = chdir(pw->pw_dir);
            if (retval == -1)
                warn("chdir(%s)", pw->pw_dir);

            execve(path, argv, env);
            perror("execve cmd");
            exit(127);
        default:;
    }

    child_pid = pid;

    for (;;) {
        pid_t wait_pid = waitpid(pid, &status, 0);
        if (wait_pid == (pid_t)-1) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            goto error;
        }
        if (wait_pid == pid)
            break;
    }

    child_pid = 0;

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
    if (child_pid)
        kill(child_pid, signal);
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
