/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
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

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <signal.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <grp.h>
#include <err.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <qubes-gui-protocol.h>
#include <qubes-xorg-tray-defs.h>
#include "xdriver-shm-cmd.h"
#include "txrx.h"
#include "list.h"
#include "error.h"
#include "encoding.h"
#include "unix-addr.h"
#include <libvchan.h>
#include <poll.h>
#include "unistr.h"


#include <linux/input.h>
#include <linux/uinput.h>
#include <time.h>



/* Time in milliseconds after which the clipboard data should be wiped */
#define CLIPBOARD_WIPE_TIME 60000

/* Get the size of an array.  Error out on pointers. */
#define QUBES_ARRAY_SIZE(x) (0 * sizeof(struct { \
    uint8_t tried_to_compute_number_of_array_elements_in_a_pointer: \
        8 - 16*__builtin_types_compatible_p(__typeof__(x), __typeof__(&((x)[0]))); \
    }) + sizeof(x)/sizeof((x)[0]))
#define SOCKET_ADDRESS  "/var/run/xf86-qubes-socket"

/* Supported protocol version */

#define PROTOCOL_VERSION_MAJOR 1
#define PROTOCOL_VERSION_MINOR 8
#define PROTOCOL_VERSION (PROTOCOL_VERSION_MAJOR << 16 | PROTOCOL_VERSION_MINOR)

#if !(PROTOCOL_VERSION_MAJOR == QUBES_GUID_PROTOCOL_VERSION_MAJOR && \
      PROTOCOL_VERSION_MINOR <= QUBES_GUID_PROTOCOL_VERSION_MINOR)
#  error Incompatible qubes-gui-protocol.h.
#endif

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

static int damage_event, damage_error;
static int xfixes_event, xfixes_error;
/* from gui-common/error.c */
extern int print_x11_errors;

static char **saved_argv;

typedef struct {
    Display *display;
    int screen;            /* shortcut to the default screen */
    Window root_win;       /* root attributes */
    GC context;
    Atom wmDeleteWindow;   /* Atom: WM_DELETE_WINDOW */
    Atom wmProtocols;      /* Atom: WM_PROTOCOLS */
    Atom wm_hints;         /* Atom: WM_HINTS */
    Atom wm_class;         /* Atom: WM_CLASS */
    Atom tray_selection;   /* Atom: _NET_SYSTEM_TRAY_SELECTION_S<creen number> */
    Atom tray_opcode;      /* Atom: _NET_SYSTEM_TRAY_OPCODE */
    Atom xembed_info;      /* Atom: _XEMBED_INFO */
    Atom utf8_string_atom; /* Atom: UTF8_STRING */
    Atom wm_state;         /* Atom: WM_STATE */
    Atom net_wm_state;     /* Atom: _NET_WM_STATE */
    Atom wm_state_fullscreen; /* Atom: _NET_WM_STATE_FULLSCREEN */
    Atom wm_state_demands_attention; /* Atom: _NET_WM_STATE_DEMANDS_ATTENTION */
    Atom wm_take_focus;    /* Atom: WM_TAKE_FOCUS */
    Atom net_wm_name;      /* Atom: _NET_WM_NAME */
    Atom wm_normal_hints;  /* Atom: WM_NORMAL_HINTS */
    Atom clipboard;        /* Atom: CLIPBOARD */
    Atom targets;          /* Atom: TARGETS */
    Atom qprop;            /* Atom: QUBES_SELECTION */
    Atom compound_text;    /* Atom: COMPOUND_TEXT */
    Atom xembed;           /* Atom: _XEMBED */
    int xserver_fd;
    int xserver_listen_fd;
    libvchan_t *vchan;
    Window stub_win;    /* window for clipboard operations and to simulate LeaveNotify events */
    unsigned char *clipboard_data;
    unsigned int clipboard_data_len;
    Time clipboard_last_access;
    bool clipboard_wipe;
    int log_level;
    int sync_all_modifiers;
    int composite_redirect_automatic;
    pid_t x_pid;
    uint32_t domid;
    uint32_t protocol_version;
    Time time;
    int uinput_fd;
    int created_input_device;
    uint8_t last_known_modifier_states;
} Ghandles;

struct window_data {
    int is_docked; /* is it docked icon window */
    XID embeder;   /* for docked icon points embeder window */
    int input_hint; /* the window should get input focus - False=Never */
    int support_delete_window;
    int support_take_focus;
    int window_dump_pending; /* send MSG_WINDOW_DUMP at next damage notification */
};

struct embeder_data {
    XID icon_window;
};

static struct genlist *windows_list;
static struct genlist *embeder_list;
static Ghandles *ghandles_for_vchan_reinitialize;

#define SKIP_NONMANAGED_WINDOW do {                                    \
    if (!list_lookup(windows_list, window)) {                          \
        if (g->log_level > 0)                                          \
            fprintf(stderr, "Skipping unmanaged window 0x%lx\n",       \
                    window);                                           \
        return;                                                        \
    }                                                                  \
} while (0)

/* Cursor name translation. See X11/cursorfont.h. */

struct supported_cursor {
    const char *name;
    uint32_t cursor_id;
};

static struct supported_cursor supported_cursors[] = {
    /* Names as defined by Xlib. Most programs will use these. */
    { "X_cursor",            XC_X_cursor },
    { "arrow",               XC_arrow },
    { "based_arrow_down",    XC_based_arrow_down },
    { "based_arrow_up",      XC_based_arrow_up },
    { "boat",                XC_boat },
    { "bogosity",            XC_bogosity },
    { "bottom_left_corner",  XC_bottom_left_corner },
    { "bottom_right_corner", XC_bottom_right_corner },
    { "bottom_side",         XC_bottom_side },
    { "bottom_tee",          XC_bottom_tee },
    { "box_spiral",          XC_box_spiral },
    { "center_ptr",          XC_center_ptr },
    { "circle",              XC_circle },
    { "clock",               XC_clock },
    { "coffee_mug",          XC_coffee_mug },
    { "cross",               XC_cross },
    { "cross_reverse",       XC_cross_reverse },
    { "crosshair",           XC_crosshair },
    { "diamond_cross",       XC_diamond_cross },
    { "dot",                 XC_dot },
    { "dotbox",              XC_dotbox },
    { "double_arrow",        XC_double_arrow },
    { "draft_large",         XC_draft_large },
    { "draft_small",         XC_draft_small },
    { "draped_box",          XC_draped_box },
    { "exchange",            XC_exchange },
    { "fleur",               XC_fleur },
    { "gobbler",             XC_gobbler },
    { "gumby",               XC_gumby },
    { "hand1",               XC_hand1 },
    { "hand2",               XC_hand2 },
    { "heart",               XC_heart },
    { "icon",                XC_icon },
    { "iron_cross",          XC_iron_cross },
    { "left_ptr",            XC_left_ptr },
    { "left_side",           XC_left_side },
    { "left_tee",            XC_left_tee },
    { "leftbutton",          XC_leftbutton },
    { "ll_angle",            XC_ll_angle },
    { "lr_angle",            XC_lr_angle },
    { "man",                 XC_man },
    { "middlebutton",        XC_middlebutton },
    { "mouse",               XC_mouse },
    { "pencil",              XC_pencil },
    { "pirate",              XC_pirate },
    { "plus",                XC_plus },
    { "question_arrow",      XC_question_arrow },
    { "right_ptr",           XC_right_ptr },
    { "right_side",          XC_right_side },
    { "right_tee",           XC_right_tee },
    { "rightbutton",         XC_rightbutton },
    { "rtl_logo",            XC_rtl_logo },
    { "sailboat",            XC_sailboat },
    { "sb_down_arrow",       XC_sb_down_arrow },
    { "sb_h_double_arrow",   XC_sb_h_double_arrow },
    { "sb_left_arrow",       XC_sb_left_arrow },
    { "sb_right_arrow",      XC_sb_right_arrow },
    { "sb_up_arrow",         XC_sb_up_arrow },
    { "sb_v_double_arrow",   XC_sb_v_double_arrow },
    { "shuttle",             XC_shuttle },
    { "sizing",              XC_sizing },
    { "spider",              XC_spider },
    { "spraycan",            XC_spraycan },
    { "star",                XC_star },
    { "target",              XC_target },
    { "tcross",              XC_tcross },
    { "top_left_arrow",      XC_top_left_arrow },
    { "top_left_corner",     XC_top_left_corner },
    { "top_right_corner",    XC_top_right_corner },
    { "top_side",            XC_top_side },
    { "top_tee",             XC_top_tee },
    { "trek",                XC_trek },
    { "ul_angle",            XC_ul_angle },
    { "umbrella",            XC_umbrella },
    { "ur_angle",            XC_ur_angle },
    { "watch",               XC_watch },
    { "xterm",               XC_xterm },

    /* Chromium (and derived projects) use different names.
     * See: https://github.com/chromium/chromium/blob/ccd149af47315e4c6f2fc45d55be1b271f39062c/ui/base/cursor/cursor_loader_x11.cc#L25
     */
    { "pointer",    XC_hand2 },
    { "progress",   XC_watch },
    { "wait",       XC_watch },
    { "cell",       XC_plus },
    { "all-scroll", XC_fleur },
    { "v-scroll",   XC_fleur },
    { "h-scroll",   XC_fleur },
    { "crosshair",  XC_cross },
    { "text",       XC_xterm },
    // { "not-allowed", x11::None },
    { "grabbing",   XC_hand2 },
    { "col-resize", XC_sb_h_double_arrow },
    { "row-resize", XC_sb_v_double_arrow },
    { "n-resize",   XC_top_side },
    { "e-resize",   XC_right_side },
    { "s-resize",   XC_bottom_side },
    { "w-resize",   XC_left_side },
    { "ne-resize",  XC_top_right_corner },
    { "nw-resize",  XC_top_left_corner },
    { "se-resize",  XC_bottom_right_corner },
    { "sw-resize",  XC_bottom_left_corner },
    { "ew-resize",  XC_sb_h_double_arrow },
    { "ns-resize",  XC_sb_v_double_arrow },
    // { "nesw-resize",x11::None},
    // { "nwse-resize",x11::None},
    { "dnd-none",   XC_hand2 },
    { "dnd-move",   XC_hand2 },
    { "dnd-copy",   XC_hand2 },
    { "dnd-link",   XC_hand2 },
};

#define NUM_SUPPORTED_CURSORS (QUBES_ARRAY_SIZE(supported_cursors))

static int compare_supported_cursors(const void *a, const void *b) {
    return strcmp(((const struct supported_cursor *)a)->name,
                  ((const struct supported_cursor *)b)->name);
}


static void send_event(Ghandles * g, const struct input_event *iev) {
    int status = write(g->uinput_fd, iev, sizeof(struct input_event));
    if ( status < 0 ) {
        if (g->log_level > 0) {
            fprintf(stderr, "write failed, falling back to xdriver. TYPE:%d CODE:%d VALUE: %d WRITE ERROR CODE: %d\n", iev->type, iev->code, iev->value, status);
        }
        g->created_input_device = 0;
    }
    
    // send syn
    const struct input_event syn = {.type = EV_SYN, .code = 0, .value = 0};
    status = write(g->uinput_fd, &(syn), sizeof(struct input_event));
    
    if ( status < 0 ) {
        if (g->log_level > 0) {
            fprintf(stderr, "writing SYN failed, falling back to xdriver. WRITE ERROR CODE: %d\n", status);
        }
        g->created_input_device = 0;
    }
}


static void send_wmname(Ghandles * g, XID window);
static void send_wmnormalhints(Ghandles * g, XID window, int ignore_fail);
static void send_wmclass(Ghandles * g, XID window, int ignore_fail);
static void send_pixmap_grant_refs(Ghandles * g, XID window);
static void retrieve_wmhints(Ghandles * g, XID window, int ignore_fail);
static void retrieve_wmprotocols(Ghandles * g, XID window, int ignore_fail);

static void process_xevent_damage(Ghandles * g, XID window,
        int x, int y, int width, int height)
{
    struct msg_shmimage mx;
    struct msg_hdr hdr;
    struct genlist *l;
    struct window_data *wd;

    l = list_lookup(windows_list, window);
    if (!l)
        return;

    wd = l->data;
    if (wd->window_dump_pending) {
        send_pixmap_grant_refs(g, window);
        wd->window_dump_pending = False;
    }

    hdr.type = MSG_SHMIMAGE;
    hdr.window = window;
    mx.x = x;
    mx.y = y;
    mx.width = width;
    mx.height = height;
    write_message(g->vchan, hdr, mx);
}

static void send_cursor(Ghandles *g, XID window, uint32_t cursor)
{
    struct msg_hdr hdr;
    struct msg_cursor msg;

    hdr.type = MSG_CURSOR;
    hdr.window = window;
    msg.cursor = cursor;
    write_message(g->vchan, hdr, msg);
}

static uint32_t find_cursor(Ghandles *g, Atom atom)
{
    char *name;
    struct supported_cursor c;
    struct supported_cursor *found;

    if (atom == None)
        return CURSOR_DEFAULT;

    name = XGetAtomName(g->display, atom);
    if (!name)
        return CURSOR_DEFAULT;

    c.name = name;
    found = bsearch(&c, supported_cursors, NUM_SUPPORTED_CURSORS,
                    sizeof(supported_cursors[0]),
                    compare_supported_cursors);
    XFree(name);

    if (found) {
        uint32_t cursor = CURSOR_X11 + found->cursor_id;
        assert(cursor < CURSOR_X11_MAX);
        return cursor;
    }

    return CURSOR_DEFAULT;
}

static void process_xevent_cursor(Ghandles *g, XFixesCursorNotifyEvent *ev)
{
    if (ev->subtype == XFixesDisplayCursorNotify) {
        /* The event from XFixes specifies only the root window, so we need to
         * find out the window under pointer.
         */
        Window root, window_under_pointer;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        Bool ret;
        int cursor;

        ret = XQueryPointer(g->display, ev->window, &root,
                            &window_under_pointer,
                            &root_x, &root_y, &win_x, &win_y, &mask);
        if (!ret || window_under_pointer == None)
            return;

        if (!list_lookup(windows_list, window_under_pointer))
            return;

        cursor = find_cursor(g, ev->cursor_name);
        send_cursor(g, window_under_pointer, cursor);
    }
}

static void process_xevent_createnotify(Ghandles * g, XCreateWindowEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_create crt;
    struct window_data *wd;

    XWindowAttributes attr;
    int ret;
    ret = XGetWindowAttributes(g->display, ev->window, &attr);
    if (ret != 1) {
        fprintf(stderr, "XGetWindowAttributes for 0x%lx failed in "
                "handle_create, ret=0x%x\n", ev->window, ret);
        return;
    }

    if (g->log_level > 0)
        fprintf(stderr, "Create for 0x%lx class 0x%x\n",
                ev->window, attr.class);
    if (list_lookup(windows_list, ev->window)) {
        fprintf(stderr, "CREATE for already existing 0x%lx\n", ev->window);
        return;
    }

    if (ev->parent != g->root_win) {
        /* GUI daemon no longer supports this */
        fprintf(stderr,
                "CREATE with non-root parent window 0x%lx\n", ev->parent);
        return;
    }

    if (list_lookup(embeder_list, ev->window)) {
        /* ignore CreateNotify for embeder window */
        if (g->log_level > 1)
            fprintf(stderr, "CREATE for embeder 0x%lx\n", ev->window);
        return;
    }

    /* Initialize window_data structure */
    wd = (struct window_data*)malloc(sizeof(struct window_data));
    if (!wd) {
        fprintf(stderr, "OUT OF MEMORY\n");
        return;
    }
    /* Default values for window_data. By default, window should receive InputFocus events */
    wd->is_docked = False;
    wd->input_hint = True;
    wd->support_delete_window = False;
    wd->support_take_focus = False;
    wd->window_dump_pending = False;
    list_insert(windows_list, ev->window, wd);

    if (attr.border_width > 0) {
        XSetWindowBorderWidth(g->display, ev->window, 0);
    }

    if (attr.class != InputOnly)
        XDamageCreate(g->display, ev->window,
                XDamageReportRawRectangles);
    // the following hopefully avoids missed damage events
    XSync(g->display, False);
    XSelectInput(g->display, ev->window, PropertyChangeMask);
    hdr.type = MSG_CREATE;
    hdr.window = ev->window;
    crt.width = ev->width;
    crt.height = ev->height;
    crt.parent = ev->parent;
    crt.x = ev->x;
    crt.y = ev->y;
    crt.override_redirect = ev->override_redirect;
    write_message(g->vchan, hdr, crt);
    /* handle properties set before we process XCreateNotify */
    send_wmnormalhints(g, hdr.window, 1);
    send_wmname(g, hdr.window);
    send_wmclass(g, hdr.window, 1);
    retrieve_wmprotocols(g, hdr.window, 1);
    retrieve_wmhints(g, hdr.window, 1);
}

static void feed_xdriver(Ghandles * g, int type, int arg1, int arg2)
{
    char ans;
    ssize_t ret;
    struct xdriver_cmd cmd;

    cmd.type = type;
    cmd.arg1 = arg1;
    cmd.arg2 = arg2;
    if (write(g->xserver_fd, &cmd, sizeof(cmd)) != sizeof(cmd))
        err(1, "unix write");
    ans = '1';
    ret = read(g->xserver_fd, &ans, 1);
    if (ret != 1 || ans != '0') {
        perror("unix read");
        err(1, "read returned %zd, char read=0x%hhx\n", ret, ans);
    }
}

void send_pixmap_grant_refs(Ghandles * g, XID window)
{
    struct msg_hdr hdr;
    uint8_t *wd_msg_buf;
    size_t wd_msg_len;
    size_t rcvd;
    int ret;

    feed_xdriver(g, 'W', (int) window, 0);
    if (read(g->xserver_fd, &wd_msg_len, sizeof(wd_msg_len)) != sizeof(wd_msg_len))
        err(1, "unix read wd_msg_len");
    if (wd_msg_len == 0) {
        fprintf(stderr, "Failed to get window dump for window 0x%lx\n",
                window);
        return;
    }
    wd_msg_buf = malloc(wd_msg_len);
    if (!wd_msg_buf) {
        fprintf(stderr, "Failed to allocate memory for window dump 0x%lx\n",
                window);
        exit(1);
    }
    rcvd = 0;
    while (rcvd < wd_msg_len) {
        ret = read(g->xserver_fd, wd_msg_buf + rcvd, wd_msg_len - rcvd);
        if (ret == 0)
            errx(1, "unix read EOF");
        if (ret < 0)
            err(1, "unix read error");
        rcvd += ret;
    }
    hdr.type = MSG_WINDOW_DUMP;
    hdr.window = window;
    hdr.untrusted_len = wd_msg_len;
    write_struct(g->vchan, hdr);
    write_data(g->vchan, (char *) wd_msg_buf, wd_msg_len);
    free(wd_msg_buf);
    if (g->protocol_version < QUBES_GUID_MIN_MSG_WINDOW_DUMP_ACK)
        feed_xdriver(g, 'a', 0, 0);
}

/* return 1 on success, 0 otherwise */
static int get_net_wmname(Ghandles * g, XID window, char *outbuf, size_t bufsize) {
    Atom type_return;
    int format_return;
    unsigned long bytes_after_return, items_return;
    unsigned char *property_data;

    if (XGetWindowProperty(g->display, window, g->net_wm_name,
                0, /* offset, in 32-bit quantities */
                bufsize/4, /* length, in 32-bit quantities */
                False, /* delete */
                g->utf8_string_atom, /* req_type */
                &type_return, /* actual_type_return */
                &format_return, /* actual_format_return */
                &items_return, /* nitems_return */
                &bytes_after_return, /* bytes_after_return */
                &property_data /* prop_return */
                ) == Success) {
        if (type_return != g->utf8_string_atom) {
            if (g->log_level > 0)
                fprintf(stderr, "get_net_wmname(0x%lx): unexpected property type: 0x%lx\n",
                        window, type_return);
            /* property_data not filled in this case */
            return 0;
        }
        if (format_return != 8) {
            if (g->log_level > 0)
                fprintf(stderr, "get_net_wmname(0x%lx): unexpected format: %d\n",
                        window, format_return);
            XFree(property_data);
            return 0;
        }
        if (bytes_after_return > 0) {
            if (g->log_level > 0)
                fprintf(stderr, "get_net_wmname(0x%lx): window title too long, %ld bytes truncated\n",
                        window, bytes_after_return);
        }

        if (items_return > bufsize) {
            if (g->log_level > 0)
                fprintf(stderr, "get_net_wmname(0x%lx): too much data returned (%ld), bug?\n",
                        window, items_return);
            XFree(property_data);
            return 0;
        }
        memcpy(outbuf, property_data, items_return);
        /* make sure there is trailing \0 */
        outbuf[bufsize-1] = 0;
        XFree(property_data);
        if (g->log_level > 0)
            fprintf(stderr, "got net_wm_name=%s\n", outbuf);
        return 1;
    }
    if (g->log_level > 1)
        fprintf(stderr, "window %lx has no _NET_WM_NAME\n", window);
    return 0;
}

/* return 1 on success, 0 otherwise */
static int getwmname_tochar(Ghandles * g, XID window, char *outbuf, int bufsize)
{
    XTextProperty text_prop_return;
    char **list;
    int count;

    outbuf[0] = 0;
    if (!XGetWMName(g->display, window, &text_prop_return) ||
            !text_prop_return.value || !text_prop_return.nitems)
        return 0;
    if (Xutf8TextPropertyToTextList(g->display,
                &text_prop_return, &list,
                &count) < 0 || count <= 0
            || !*list) {
        XFree(text_prop_return.value);
        return 0;
    }
    strncat(outbuf, list[0], bufsize - 1);
    XFree(text_prop_return.value);
    XFreeStringList(list);
    if (g->log_level > 0)
        fprintf(stderr, "got wmname=%s\n", outbuf);
    return 1;
}

void send_wmname(Ghandles * g, XID window)
{
    struct msg_hdr hdr;
    struct msg_wmname msg;
    memset(&msg, 0, sizeof(msg));
    /* try _NET_WM_NAME, then fallback to WM_NAME */
    if (!get_net_wmname(g, window, msg.data, sizeof(msg.data)))
        if (!getwmname_tochar(g, window, msg.data, sizeof(msg.data)))
            return;
    if (strlen(msg.data) == sizeof(msg.data) - 1) {
        // Window title might had been longer than output buffer.
        // Trim at correct utf8 boundary and set end to U+2026 (Horizontal Ellipsis)
        msg.data[sizeof(msg.data) - 4] = 0;
        for (int i=0; i < 4; i++) {
            uint8_t * last_byte = (uint8_t *)msg.data + sizeof(msg.data) - 5 - i;
            // check for valid 1, 2, 3 or 4 byte uft8 char at the end of string
            if (
                !u8_check(last_byte, 1) ||
                !u8_check(last_byte - 1, 2) ||
                !u8_check(last_byte - 2, 3) ||
                !u8_check(last_byte - 3, 4)
            ) {
                break;
            } else {
                // trim one invalid byte at end of string
                *last_byte = 0;
            }
        }
        strncat(msg.data, "\xE2\x80\xA6", sizeof(msg.data) - 1);
    }
    hdr.window = window;
    hdr.type = MSG_WMNAME;
    write_message(g->vchan, hdr, msg);
}

/*	Retrieve the supported WM Protocols
    We don't forward the info to dom0 as we only need specific client protocols
    */
void retrieve_wmprotocols(Ghandles * g, XID window, int ignore_fail)
{
    int nitems;
    Atom *supported_protocols;
    int i;
    struct genlist *l;

    if (!((l=list_lookup(windows_list, window)) && (l->data))) {
        fprintf(stderr, "ERROR retrieve_wmprotocols: Window 0x%lx data not initialized\n", window);
        return;
    }

    if (XGetWMProtocols(g->display, window, &supported_protocols, &nitems) == 1) {
        for (i=0; i < nitems; i++) {
            if (supported_protocols[i] == g->wm_take_focus) {
                if (g->log_level > 1)
                    fprintf(stderr, "Protocol take_focus supported for Window 0x%lx\n", window);

                ((struct window_data*)l->data)->support_take_focus = True;
            } else if (supported_protocols[i] == g->wmDeleteWindow) {
                if (g->log_level > 1)
                    fprintf(stderr, "Protocol delete_window supported for Window 0x%lx\n", window);

                ((struct window_data*)l->data)->support_delete_window = True;
            }
        }
    } else {
        if (!ignore_fail)
            fprintf(stderr, "ERROR reading WM_PROTOCOLS\n");
        return;
    }
    XFree(supported_protocols);
}


/* 	Retrieve the 'real' WMHints.
    We don't forward the info to dom0 as we only need InputHint and dom0 doesn't care about it
    */
void retrieve_wmhints(Ghandles * g, XID window, int ignore_fail)
{
    XWMHints *wm_hints;
    struct genlist *l;

    if (!((l=list_lookup(windows_list, window)) && (l->data))) {
        fprintf(stderr, "ERROR retrieve_wmhints: Window 0x%lx data not initialized\n", window);
        return;
    }

    if (!(wm_hints = XGetWMHints(g->display, window))) {
        if (!ignore_fail)
            fprintf(stderr, "ERROR reading WM_HINTS\n");
        return;
    }

    if (wm_hints->flags & InputHint) {
        ((struct window_data*)l->data)->input_hint = wm_hints->input;

        if (g->log_level > 1)
            fprintf(stderr, "Received input hint 0x%x for Window 0x%lx\n", wm_hints->input, window);
    } else {
        // Default value
        if (g->log_level > 1)
            fprintf(stderr, "Received WMHints without input hint set for Window 0x%lx\n", window);
        ((struct window_data*)l->data)->input_hint = True;
    }
    XFree(wm_hints);
}

void send_wmnormalhints(Ghandles * g, XID window, int ignore_fail)
{
    struct msg_hdr hdr;
    struct msg_window_hints msg;
    XSizeHints size_hints;
    long supplied_hints;

    if (!XGetWMNormalHints
            (g->display, window, &size_hints, &supplied_hints)) {
        if (!ignore_fail)
            fprintf(stderr, "error reading WM_NORMAL_HINTS\n");
        return;
    }

    /* Nasty workaround for KDE bug affecting gnome-terminal (shrinks to minimal size) */
    /* https://bugzilla.redhat.com/show_bug.cgi?id=707664 */
    if ((size_hints.flags & (PBaseSize|PMinSize|PResizeInc)) ==
            (PBaseSize|PMinSize|PResizeInc)) {
        /* KDE incorrectly uses PMinSize when both are provided */
        if (size_hints.width_inc > 1)
            /* round up to neareset multiply of width_inc */
            size_hints.min_width =
                ((size_hints.min_width-size_hints.base_width+1) / size_hints.width_inc)
                * size_hints.width_inc + size_hints.base_width;
        if (size_hints.height_inc > 1)
            /* round up to neareset multiply of height_inc */
            size_hints.min_height =
                ((size_hints.min_height-size_hints.base_height+1) / size_hints.height_inc)
                * size_hints.height_inc + size_hints.base_height;
    }

    // pass only some hints
    msg.flags =
        size_hints.flags & (USPosition | PPosition | PMinSize | PMaxSize |
                PResizeInc | PBaseSize);
    msg.min_width = size_hints.min_width;
    msg.min_height = size_hints.min_height;
    msg.max_width = size_hints.max_width;
    msg.max_height = size_hints.max_height;
    msg.width_inc = size_hints.width_inc;
    msg.height_inc = size_hints.height_inc;
    msg.base_width = size_hints.base_width;
    msg.base_height = size_hints.base_height;
    hdr.window = window;
    hdr.type = MSG_WINDOW_HINTS;
    write_message(g->vchan, hdr, msg);
}

void send_wmclass(Ghandles * g, XID window, int ignore_fail)
{
    struct msg_hdr hdr;
    struct msg_wmclass msg;
    XClassHint class_hint;

    if (!XGetClassHint(g->display, window, &class_hint)) {
        if (!ignore_fail)
            fprintf(stderr, "error reading WM_CLASS\n");
        return;
    }

    strncpy(msg.res_class, class_hint.res_class, sizeof(msg.res_class)-1);
    msg.res_class[sizeof(msg.res_class)-1] = '\0';
    strncpy(msg.res_name, class_hint.res_name, sizeof(msg.res_name)-1);
    msg.res_name[sizeof(msg.res_name)-1] = '\0';
    XFree(class_hint.res_class);
    XFree(class_hint.res_name);
    hdr.window = window;
    hdr.type = MSG_WMCLASS;
    write_message(g->vchan, hdr, msg);
}


static inline uint32_t flags_from_atom(Ghandles * g, Atom a) {
    if (a == g->wm_state_fullscreen)
        return WINDOW_FLAG_FULLSCREEN;
    else if (a == g->wm_state_demands_attention)
        return WINDOW_FLAG_DEMANDS_ATTENTION;
    else {
        /* ignore unsupported states */
    }
    return 0;
}

static void send_window_state(Ghandles * g, XID window)
{
    int ret;
    unsigned i;
    Atom *state_list;
    Atom act_type;
    int act_fmt;
    unsigned long nitems, bytesleft;
    struct msg_hdr hdr;
    struct msg_window_flags flags;

    /* FIXME: only first 10 elements are parsed */
    ret = XGetWindowProperty(g->display, window, g->net_wm_state, 0, 10,
            False, XA_ATOM, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&state_list);
    if (ret != Success)
        return;

    flags.flags_set = 0;
    flags.flags_unset = 0;
    for (i=0; i < nitems; i++) {
        flags.flags_set |= flags_from_atom(g, state_list[i]);
    }
    hdr.window = window;
    hdr.type = MSG_WINDOW_FLAGS;
    write_message(g->vchan, hdr, flags);
    XFree(state_list);
}

static void process_xevent_map(Ghandles * g, XID window)
{
    XWindowAttributes attr;
    long new_wm_state[2];
    struct msg_hdr hdr;
    struct msg_map_info map_info;
    Window transient;
    struct window_data *wd;
    SKIP_NONMANAGED_WINDOW;

    wd = list_lookup(windows_list, window)->data;

    if (g->log_level > 1)
        fprintf(stderr, "MAP for window 0x%lx\n", window);
    wd->window_dump_pending = True;
    send_window_state(g, window);
    XGetWindowAttributes(g->display, window, &attr);
    if (XGetTransientForHint(g->display, window, &transient))
        map_info.transient_for = transient;
    else
        map_info.transient_for = 0;
    map_info.override_redirect = attr.override_redirect;
    hdr.type = MSG_MAP;
    hdr.window = window;
    write_message(g->vchan, hdr, map_info);
    send_wmname(g, window);

    if (!attr.override_redirect) {
        /* WM_STATE is always set to normal */
        new_wm_state[0] = NormalState; /* state */
        new_wm_state[1] = None;        /* icon */
        XChangeProperty(g->display, window, g->wm_state, g->wm_state, 32, PropModeReplace, (unsigned char *)new_wm_state, 2);
    }
}

static void process_xevent_unmap(Ghandles * g, XID window)
{
    struct msg_hdr hdr;
    SKIP_NONMANAGED_WINDOW;

    if (g->log_level > 1)
        fprintf(stderr, "UNMAP for window 0x%lx\n", window);
    hdr.type = MSG_UNMAP;
    hdr.window = window;
    hdr.untrusted_len = 0;
    write_struct(g->vchan, hdr);
    XDeleteProperty(g->display, window, g->wm_state);
    XDeleteProperty(g->display, window, g->net_wm_state);
}

static void process_xevent_destroy(Ghandles * g, XID window)
{
    struct msg_hdr hdr;
    struct genlist *l;
    /* embeders are not manged windows, so must be handled before SKIP_NONMANAGED_WINDOW */
    if ((l = list_lookup(embeder_list, window))) {
        if (l->data) {
            free(l->data);
        }
        list_remove(l);
    }

    SKIP_NONMANAGED_WINDOW;
    if (g->log_level > 0)
        fprintf(stderr, "handle destroy 0x%lx\n", window);
    hdr.type = MSG_DESTROY;
    hdr.window = window;
    hdr.untrusted_len = 0;
    write_struct(g->vchan, hdr);
    l = list_lookup(windows_list, window);
    if (l->data) {
        if (((struct window_data*)l->data)->is_docked) {
            XDestroyWindow(g->display, ((struct window_data*)l->data)->embeder);
        }
        free(l->data);
    }
    list_remove(l);
}

static void process_xevent_configure(Ghandles * g, XID window,
        XConfigureEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_configure conf;
    struct genlist *l;
    /* SKIP_NONMANAGED_WINDOW; */
    if (!(l=list_lookup(windows_list, window))) {
        /* if not real managed window, check if this is embeder for another window */
        struct genlist *e;
        if ((e=list_lookup(embeder_list, window))) {
            window = ((struct embeder_data*)e->data)->icon_window;
            if (!list_lookup(windows_list, window))
                /* probably icon window have just destroyed, so ignore message */
                /* "l" not updated intentionally - when configure notify comes
                 * from the embeder, it should be passed to dom0 (in most cases as
                 * ACK for earlier configure request) */
                return;
        } else {
            /* ignore not managed windows */
            return;
        }
    }

    if (g->log_level > 1)
        fprintf(stderr,
                "handle configure event 0x%lx w=%d h=%d ovr=%d\n",
                window, ev->width, ev->height,
                ev->override_redirect);
    if (l && l->data && ((struct window_data*)l->data)->is_docked) {
        /* for docked icon, ensure that it fills embeder window; don't send any
         * message to dom0 - it will be done for embeder itself*/
        XWindowAttributes attr;
        int ret;

        ret = XGetWindowAttributes(g->display, ((struct window_data*)l->data)->embeder, &attr);
        if (ret != 1) {
            fprintf(stderr,
                    "XGetWindowAttributes for 0x%lx failed in "
                    "handle_xevent_configure, ret=0x%x\n", ((struct window_data*)l->data)->embeder, ret);
            return;
        }
        if (ev->x != 0 || ev->y != 0 || ev->width != attr.width || ev->height != attr.height) {
            XMoveResizeWindow(g->display, window, 0, 0, attr.width, attr.height);
        }
        return;
    }

    if (ev->border_width > 0) {
        XSetWindowBorderWidth(g->display, window, 0);
    }

    hdr.type = MSG_CONFIGURE;
    hdr.window = window;
    conf.x = ev->x;
    conf.y = ev->y;
    conf.width = ev->width;
    conf.height = ev->height;
    conf.override_redirect = ev->override_redirect;
    write_message(g->vchan, hdr, conf);
    send_pixmap_grant_refs(g, window);
}

static void send_clipboard_data(libvchan_t *vchan, XID window, char *data, uint32_t len, int protocol_version)
{
    struct msg_hdr hdr;
    hdr.type = MSG_CLIPBOARD_DATA;
    hdr.window = window;
    if ((protocol_version < QUBES_GUID_MIN_CLIPBOARD_4X) && (len > MAX_CLIPBOARD_SIZE)) {
        // The dumb case. Truncate the data to the old size. User might lose
        // some inter-vm clipboard data without being notified.
        len = MAX_CLIPBOARD_SIZE;
    } else if (len > MAX_CLIPBOARD_BUFFER_SIZE + 1) {
        // xside is capable of receiving (up to) 4X of the previous size.
        // it is also smarter. send one byte over the new buffer limit.
        // A simple sign for xside to reject it.
        len = MAX_CLIPBOARD_BUFFER_SIZE + 1;
    }
    hdr.untrusted_len = len;
    write_struct(vchan, hdr);
    write_data(vchan, (char *) data, len);
}

static void handle_targets_list(Ghandles * g, unsigned char *data, int len)
{
    Atom *atoms = (Atom *) data;
    int i;
    int have_utf8 = 0;
    if (g->log_level > 1)
        fprintf(stderr, "target list data size %d\n", len);
    for (i = 0; i < len; i++) {
        if (atoms[i] == g->utf8_string_atom)
            have_utf8 = 1;
        if (g->log_level > 1)
            fprintf(stderr, "supported 0x%lx %s\n",
                    atoms[i], XGetAtomName(g->display,
                        atoms[i]));
    }
    XConvertSelection(g->display, g->clipboard,
            have_utf8 ? g->utf8_string_atom : XA_STRING, g->qprop,
            g->stub_win, g->time);
}

static void process_xevent_selection(Ghandles * g, XSelectionEvent * ev)
{
    int format, result;
    Atom type;
    unsigned long len, bytes_left, dummy;
    unsigned char *data;
    g->time = ev->time;

    if (g->log_level > 0)
        fprintf(stderr, "selection event, target=%s\n",
                XGetAtomName(g->display, ev->target));
    if (ev->requestor != g->stub_win || ev->property != g->qprop)
        return;
    XGetWindowProperty(g->display, ev->requestor, g->qprop, 0, 0, 0,
            AnyPropertyType, &type, &format, &len,
            &bytes_left, &data);
    if (bytes_left <= 0)
        return;
    result =
        XGetWindowProperty(g->display, ev->requestor, g->qprop, 0,
                bytes_left, 0,
                AnyPropertyType, &type,
                &format, &len, &dummy, &data);
    if (result != Success)
        return;

    if (ev->target == g->targets)
        handle_targets_list(g, data, len);
    // If we receive TARGETS atom in response for TARGETS query, let's assume
    // that UTF8 is supported.
    // this is workaround for Opera web browser...
    else if (ev->target == XA_ATOM && len >= 4 && len <= 8 &&
            // compare only first 4 bytes
            *((unsigned *) data) == g->targets)
        XConvertSelection(g->display, g->clipboard,
                g->utf8_string_atom, g->qprop,
                g->stub_win, ev->time);
    else
        if (type == XInternAtom(g->display, "INCR", False)) {
            char INCR_WARNING[] =
                "Qube clipboard size over 256KiB and X11 INCR protocol support is not implemented!";
            send_clipboard_data(g->vchan, g->stub_win, (char *) &INCR_WARNING,
                                sizeof(INCR_WARNING)-1, g->protocol_version);
        } else {
            send_clipboard_data(g->vchan, g->stub_win, (char *) data, len,
                                g->protocol_version);
            /* even if the clipboard owner does not support UTF8 and we requested
            XA_STRING, it is fine - ascii is legal UTF8 */
        }
    XFree(data);
}

static void process_xevent_selection_req(Ghandles * g,
        XSelectionRequestEvent * req)
{
    XSelectionEvent resp;
    int convert_style = XConverterNotFound;
    g->time = req->time;

    if (g->clipboard_wipe &&
            g->time > g->clipboard_last_access + CLIPBOARD_WIPE_TIME) {
        if (g->log_level > 0)
            fprintf(stderr, "wiping %ldms old clipboard data\n",
                    g->time - g->clipboard_last_access);
        g->clipboard_data[0] = '\x00';
        g->clipboard_data_len = 1;
    }

    if (g->log_level > 0)
        fprintf(stderr, "selection req event, target=%s\n",
                XGetAtomName(g->display, req->target));
    resp.property = None;
    if (req->target == g->targets) {
        Atom tmp[4] = { XA_STRING, g->targets, g->utf8_string_atom, g->compound_text };
        XChangeProperty(g->display, req->requestor, req->property,
                XA_ATOM, 32, PropModeReplace,
                (unsigned char *)
                tmp, sizeof(tmp) / sizeof(tmp[0]));
        resp.property = req->property;
    }
    if (req->target == XA_STRING)
        convert_style = XTextStyle;
    else if (req->target == g->compound_text)
        convert_style = XCompoundTextStyle;
    else if (req->target == g->utf8_string_atom)
        convert_style = XUTF8StringStyle;
    if (convert_style != XConverterNotFound) {
        XTextProperty ct;
        char empty_string[1] = { '\0' };
        char *ptr[] = { empty_string };
        // Workaround for an Xlib bug: Xutf8TextListToTextProperty mangles
        // certain characters, so we check for UTF-8 validity ourselves and use
        // XStringListToTextProperty.  If we fail a UTF-8 validity check, ptr[0]
        // is left pointing to an empty string, which is safe.
        //
        // We don’t return immediately because users might (reasonably)
        // assume that any sensitive data previously on the clipboard
        // (such as passwords) has been overwritten.
        if (convert_style == XUTF8StringStyle &&
            !is_valid_clipboard_string_from_vm((unsigned char *)g->clipboard_data))
            fputs("Invalid clipboard data from VM\n", stderr);
        else
            ptr[0] = (char *) g->clipboard_data;
        if (!XStringListToTextProperty(ptr, 1, &ct)) {
            fputs("Out of memory in Xutf8TextListToTextProperty()\n", stderr);
            return;
        }
        ct.encoding = req->target;
        XSetTextProperty(g->display, req->requestor, &ct,
                req->property);
        XFree(ct.value);
        resp.property = req->property;
    }

    if (resp.property == None)
        fprintf(stderr,
                "Not supported selection_req target 0x%lx %s\n",
                req->target, XGetAtomName(g->display, req->target));
    resp.type = SelectionNotify;
    resp.display = req->display;
    resp.requestor = req->requestor;
    resp.selection = req->selection;
    resp.target = req->target;
    resp.time = req->time;
    g->clipboard_last_access = g->time;
    XSendEvent(g->display, req->requestor, 0, 0, (XEvent *) & resp);
}

static void process_xevent_property(Ghandles * g, XID window, XPropertyEvent * ev)
{
    g->time = ev->time;
    SKIP_NONMANAGED_WINDOW;
    if (g->log_level > 1)
        fprintf(stderr, "handle property %s for window 0x%lx\n",
                XGetAtomName(g->display, ev->atom), ev->window);
    if (ev->atom == XA_WM_NAME)
        send_wmname(g, window);
    else if (ev->atom == g->net_wm_name)
        send_wmname(g, window);
    else if (ev->atom == g->wm_normal_hints)
        send_wmnormalhints(g, window, 0);
    else if (ev->atom == g->wm_class)
        send_wmclass(g, window, 0);
    else if (ev->atom == g->wm_hints)
        retrieve_wmhints(g,window, 0);
    else if (ev->atom == g->wmProtocols)
        retrieve_wmprotocols(g,window, 0);
    else if (ev->atom == g->xembed_info) {
        struct genlist *l = list_lookup(windows_list, window);
        Atom act_type;
        unsigned long nitems, bytesafter;
        unsigned char *data;
        int ret, act_fmt;

        if (!l->data || !((struct window_data*)l->data)->is_docked)
            /* ignore _XEMBED_INFO change on non-docked windows */
            return;
        ret = XGetWindowProperty(g->display, window, g->xembed_info, 0, 2, False,
                g->xembed_info, &act_type, &act_fmt, &nitems, &bytesafter,
                &data);
        if (ret && act_type == g->xembed_info && nitems == 2) {
            if (((int*)data)[1] & XEMBED_MAPPED)
                XMapWindow(g->display, window);
            else
                XUnmapWindow(g->display, window);
        }
        if (ret == Success && nitems > 0)
            XFree(data);
    }
}

static void process_xevent_message(Ghandles * g, XClientMessageEvent * ev)
{
    if (g->log_level > 1)
        fprintf(stderr, "handle message %s to window 0x%lx\n",
                XGetAtomName(g->display, ev->message_type), ev->window);
    if (ev->message_type == g->tray_opcode) {
        XClientMessageEvent resp;
        Window w;
        int ret;
        struct msg_hdr hdr;
        Atom act_type;
        int act_fmt;
        int mapwindow = 0;
        unsigned long nitems, bytesafter;
        unsigned char *data;
        struct genlist *l;
        struct window_data *wd;
        struct embeder_data *ed;

        switch (ev->data.l[1]) {
            case SYSTEM_TRAY_REQUEST_DOCK:
                w = ev->data.l[2];

                if (!(l=list_lookup(windows_list, w))) {
                    fprintf(stderr, "ERROR process_xevent_message: Window 0x%lx not initialized\n", w);
                    return;
                }
                if (g->log_level > 0)
                    fprintf(stderr, "tray request dock for window 0x%lx\n", w);
                ret = XGetWindowProperty(g->display, w, g->xembed_info, 0, 2,
                        False, g->xembed_info, &act_type, &act_fmt, &nitems,
                        &bytesafter, &data);
                if (ret != Success) {
                    fprintf(stderr, "failed to get window property, probably window doesn't longer exists\n");
                    return;
                }
                if (act_type != g->xembed_info) {
                    fprintf(stderr, "window 0x%x havn't proper _XEMBED_INFO property, assuming defaults (workaround for buggy applications)\n", (unsigned int)w);
                }
                if (act_type == g->xembed_info && nitems == 2) {
                    mapwindow = ((int*)data)[1] & XEMBED_MAPPED;
                    /* TODO: handle version */
                }
                if (ret == Success && nitems > 0)
                    XFree(data);

                if (!(l->data)) {
                    fprintf(stderr, "ERROR process_xevent_message: Window 0x%lx data not initialized\n", w);
                    return;
                }
                wd = (struct window_data*)(l->data);
                /* TODO: error checking */
                wd->embeder = XCreateSimpleWindow(g->display, g->root_win,
                        0, 0, 32, 32, /* default icon size, will be changed by dom0 */
                        0, BlackPixel(g->display,
                            g->screen),
                        WhitePixel(g->display,
                            g->screen));
                wd->is_docked=True;
                if (g->log_level > 1)
                    fprintf(stderr, " created embeder 0x%lx\n", wd->embeder);
                XSelectInput(g->display, wd->embeder, SubstructureNotifyMask);
                ed = (struct embeder_data*)malloc(sizeof(struct embeder_data));
                if (!ed) {
                    fprintf(stderr, "OUT OF MEMORY\n");
                    return;
                }
                ed->icon_window = w;
                list_insert(embeder_list, wd->embeder, ed);

                ret = XReparentWindow(g->display, w, wd->embeder, 0, 0);
                if (ret != 1) {
                    fprintf(stderr,
                            "XReparentWindow for 0x%lx failed in "
                            "handle_dock, ret=0x%x\n", w, ret);
                    return;
                }

                memset(&resp, 0, sizeof(resp));
                resp.type = ClientMessage;
                resp.window = w;
                resp.message_type = g->xembed;
                resp.format = 32;
                resp.data.l[0] = ev->data.l[0];
                resp.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
                resp.data.l[3] = wd->embeder;
                resp.data.l[4] = 0; /* TODO: handle version; GTK+ uses version 1, but spec says the latest is 0 */
                resp.display = g->display;
                XSendEvent(resp.display, resp.window, False,
                        NoEventMask, (XEvent *) & ev);
                XRaiseWindow(g->display, w);
                if (mapwindow)
                    XMapRaised(g->display, resp.window);
                XMapWindow(g->display, wd->embeder);
                XLowerWindow(g->display, wd->embeder);
                XMoveWindow(g->display, w, 0, 0);
                /* force refresh of window content */
                XClearWindow(g->display, wd->embeder);
                XClearArea(g->display, w, 0, 0, 32, 32, True); /* XXX defult size once again */
                XSync(g->display, False);

                hdr.type = MSG_DOCK;
                hdr.window = w;
                hdr.untrusted_len = 0;
                write_struct(g->vchan, hdr);
                break;
            default:
                fprintf(stderr, "unhandled tray opcode: %ld\n",
                        ev->data.l[1]);
        }
    } else if (ev->message_type == g->net_wm_state) {
        struct msg_hdr hdr;
        struct msg_window_flags msg;

        /* SKIP_NONMANAGED_WINDOW */
        if (!list_lookup(windows_list, ev->window)) return;

        msg.flags_set = 0;
        msg.flags_unset = 0;
        if (ev->data.l[0] == 0) { /* remove/unset property */
            msg.flags_unset |= flags_from_atom(g, ev->data.l[1]);
            msg.flags_unset |= flags_from_atom(g, ev->data.l[2]);
        } else if (ev->data.l[0] == 1) { /* add/set property */
            msg.flags_set |= flags_from_atom(g, ev->data.l[1]);
            msg.flags_set |= flags_from_atom(g, ev->data.l[2]);
        } else if (ev->data.l[0] == 2) { /* toggle property */
            fprintf(stderr, "toggle window 0x%lx property %s not supported, "
                    "please report it with the application name\n", ev->window,
                    XGetAtomName(g->display, ev->data.l[1]));
        } else {
            fprintf(stderr, "invalid window state command (%ld) for window 0x%lx"
                    "report with application name\n", ev->data.l[0], ev->window);
        }
        hdr.window = ev->window;
        hdr.type = MSG_WINDOW_FLAGS;
        write_message(g->vchan, hdr, msg);
    }
}

static void process_xevent(Ghandles * g)
{
    XDamageNotifyEvent *dev;
    XEvent event_buffer;
    XNextEvent(g->display, &event_buffer);
    switch (event_buffer.type) {
        case CreateNotify:
            process_xevent_createnotify(g, (XCreateWindowEvent *)
                    & event_buffer);
            break;
        case DestroyNotify:
            process_xevent_destroy(g,
                    event_buffer.xdestroywindow.window);
            break;
        case MapNotify:
            process_xevent_map(g, event_buffer.xmap.window);
            break;
        case UnmapNotify:
            process_xevent_unmap(g, event_buffer.xmap.window);
            break;
        case ConfigureNotify:
            process_xevent_configure(g,
                    event_buffer.xconfigure.window,
                    (XConfigureEvent *) &
                    event_buffer);
            break;
        case SelectionNotify:
            process_xevent_selection(g,
                    (XSelectionEvent *) &
                    event_buffer);
            break;
        case SelectionRequest:
            process_xevent_selection_req(g,
                    (XSelectionRequestEvent *) &
                    event_buffer);
            break;
        case PropertyNotify:
            process_xevent_property(g, event_buffer.xproperty.window,
                    (XPropertyEvent *) & event_buffer);
            break;
        case ClientMessage:
            process_xevent_message(g,
                    (XClientMessageEvent *) &
                    event_buffer);
            break;
        default:
            if (event_buffer.type == (damage_event + XDamageNotify)) {
                dev = (XDamageNotifyEvent *) & event_buffer;
                g->time = dev->timestamp;
                //      fprintf(stderr, "x=%hd y=%hd gx=%hd gy=%hd w=%hd h=%hd\n",
                //        dev->area.x, dev->area.y, dev->geometry.x, dev->geometry.y, dev->area.width, dev->area.height); 
                process_xevent_damage(g, dev->drawable,
                        dev->area.x,
                        dev->area.y,
                        dev->area.width,
                        dev->area.height);
                //                      fprintf(stderr, "@");
            } else if (event_buffer.type == (xfixes_event + XFixesCursorNotify)) {
                process_xevent_cursor(
                    g,
                    (XFixesCursorNotifyEvent *) &event_buffer);
            } else if (g->log_level > 1)
                fprintf(stderr, "#");
    }

}

/* return 1 if info sent, 0 otherwise */
static int send_full_window_info(Ghandles *g, XID w, struct window_data *wd)
{
    struct msg_hdr hdr;
    struct msg_create crt;
    struct msg_configure conf;
    struct msg_map_info map_info;

    XWindowAttributes attr;
    int ret;
    Window *children_list = NULL;
    Window root;
    Window parent;
    Window transient;
    unsigned int children_count;

    const Window window_to_query = wd->is_docked ? wd->embeder : w;
    ret = XGetWindowAttributes(g->display, window_to_query, &attr);
    if (ret != 1) {
        fprintf(stderr, "XGetWindowAttributes for 0x%lx failed in "
                "send_window_state, ret=0x%x\n", w, ret);
        return 0;
    }
    ret = XQueryTree(g->display, window_to_query, &root, &parent, &children_list, &children_count);
    if (ret != 1) {
        fprintf(stderr, "XQueryTree for 0x%lx failed in "
                "send_window_state, ret=0x%x\n", w, ret);
        return 0;
    }
    if (children_list)
        XFree(children_list);
    if (parent != g->root_win) {
        fprintf(stderr, "Window 0x%x has parent 0x%x, which isn't root 0x%x.\n"
                " Presumably window has been reparented at some point.\n"
                " Skipping it.\n",
                (unsigned int)window_to_query,
                (unsigned int)parent,
                (unsigned int)g->root_win);
        return 0;
    }
    if (root != g->root_win) {
        fprintf(stderr, "Window 0x%x has root 0x%x, which isn't expected root 0x%x.\n"
                " This is rather strange and probably indicates a bug somewhere.\n"
                " Skipping it.\n",
                (unsigned int)window_to_query,
                (unsigned int)root,
                (unsigned int)g->root_win);
        return 0;
    }
    if (!XGetTransientForHint(g->display, w, &transient))
        transient = 0;

    hdr.window = w;
    hdr.type = MSG_CREATE;
    crt.width = attr.width;
    crt.height = attr.height;
    crt.parent = parent;
    crt.x = attr.x;
    crt.y = attr.y;
    crt.override_redirect = attr.override_redirect;
    write_message(g->vchan, hdr, crt);

    hdr.type = MSG_CONFIGURE;
    conf.x = attr.x;
    conf.y = attr.y;
    conf.width = attr.width;
    conf.height = attr.height;
    conf.override_redirect = attr.override_redirect;
    write_message(g->vchan, hdr, conf);
    send_pixmap_grant_refs(g, w);

    send_wmclass(g, w, 1);
    send_wmnormalhints(g, w, 1);

    if (wd->is_docked) {
        hdr.type = MSG_DOCK;
        hdr.untrusted_len = 0;
        write_struct(g->vchan, hdr);
    } else if (attr.map_state != IsUnmapped) {
        hdr.type = MSG_MAP;
        map_info.override_redirect = attr.override_redirect;
        map_info.transient_for = transient;
        write_message(g->vchan, hdr, map_info);
        send_wmname(g, w);
        send_window_state(g, w);
    }
    return 1;
}

static void send_all_windows_info(Ghandles *g) {
    struct genlist *curr = windows_list->next;
    int ret;

    feed_xdriver(g, 'A', 0, 0);
    while (curr != windows_list) {
        ret = send_full_window_info(g, curr->key, (struct window_data *)curr->data);
        curr = curr->next;
        if (!ret) {
            /* gui-daemon did not received this window, so prevent further
             * updates on it */
            if (curr->prev->data)
                free(curr->prev->data);
            list_remove(curr->prev);
        }
    }
}

static void wait_for_unix_socket(Ghandles *g)
{
    struct sockaddr_un sockname, peer;
    socklen_t addrlen;
    int prev_umask;
    struct group *qubes_group;

    /* setup listening socket only once; in case of qubes_drv reconnections,
     * simply pickup next waiting connection there (using accept below) */
    if (g->xserver_listen_fd == -1) {
        addrlen = sockaddr_un_from_path(&sockname, SOCKET_ADDRESS);
        if (addrlen == 0) {
            fprintf(stderr, "invalid socket path: %s\n", SOCKET_ADDRESS);
            exit(1);
        }

        unlink(SOCKET_ADDRESS);
        g->xserver_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

        qubes_group = getgrnam("qubes");
        if (qubes_group)
            prev_umask=umask(0007);
        else
            prev_umask=umask(0000);
        if (bind(g->xserver_listen_fd, (struct sockaddr *)&sockname, addrlen) == -1) {
            fprintf(stderr, "bind() failed\n");
            close(g->xserver_listen_fd);
            exit(1);
        }
        umask(prev_umask);
        if (qubes_group) {
            if (chown(SOCKET_ADDRESS, -1, qubes_group->gr_gid) == -1) {
                perror("chown");
                if (chmod(SOCKET_ADDRESS, 0666) == -1)
                    perror("chmod"); // ignore error here
            }
        }

        if (listen(g->xserver_listen_fd, 5) == -1) {
            perror("listen() failed\n");
            close(g->xserver_listen_fd);
            exit(1);
        }
    }

    addrlen = sizeof(peer);
    fprintf (stderr, "Waiting on %s socket...\n", SOCKET_ADDRESS);
    if (g->x_pid == (pid_t)-1) {
        fprintf(stderr, "Xorg exited in the meantime, aborting\n");
        exit(1);
    }
    g->xserver_fd = accept(g->xserver_listen_fd, (struct sockaddr *) &peer, &addrlen);
    if (g->xserver_fd == -1) {
        if (errno == EINTR && g->x_pid == (pid_t)-1)
            fprintf(stderr, "Xorg exited in the meantime, aborting\n");
        else
            perror("unix accept");
        exit(1);
    }
    fprintf (stderr, "Ok, somebody connected.\n");
}

static void mkghandles(Ghandles * g)
{
    char tray_sel_atom_name[64];

    g->xserver_listen_fd = -1;
    g->xserver_fd = -1;
    wait_for_unix_socket(g);	// wait for Xorg qubes_drv to connect to us
    do {
        g->display = XOpenDisplay(NULL);
        if (!g->display && errno != EAGAIN) {
            perror("XOpenDisplay");
            exit(1);
        }
    } while (!g->display);
    if (g->log_level > 0)
        fprintf(stderr,
                "Connection to local X server established.\n");
    g->screen = DefaultScreen(g->display);	/* get CRT id number */
    g->root_win = RootWindow(g->display, g->screen);	/* get default attributes */
    g->context = XCreateGC(g->display, g->root_win, 0, NULL);
    g->stub_win = XCreateSimpleWindow(g->display, g->root_win,
            0, 0, 1, 1,
            0, BlackPixel(g->display,
                g->screen),
            WhitePixel(g->display,
                g->screen));
    if ((unsigned)snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
            "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display)) >=
        sizeof tray_sel_atom_name)
        abort();
#define SUPPORTED_ATOMS 6
    const struct {
        Atom *const dest;
        const char *const name;
    } atoms_to_intern[] = {
        { &g->wmDeleteWindow,   "WM_DELETE_WINDOW" },
        { &g->wmProtocols,      "WM_PROTOCOLS" },
        { &g->wm_hints,         "WM_HINTS" },
        { &g->wm_class,         "WM_CLASS" },
        { &g->tray_selection,   tray_sel_atom_name },
        { &g->tray_opcode,      "_NET_SYSTEM_TRAY_OPCODE" },
        { &g->xembed_info,      "_XEMBED_INFO" },
        { &g->utf8_string_atom, "UTF8_STRING" },
        { &g->wm_state,         "WM_STATE" },
        { &g->net_wm_state,     "_NET_WM_STATE" },
        { &g->wm_state_fullscreen, "_NET_WM_STATE_FULLSCREEN" },
        { &g->wm_state_demands_attention, "_NET_WM_STATE_DEMANDS_ATTENTION" },
        { &g->wm_take_focus,    "WM_TAKE_FOCUS" },
        { &g->net_wm_name,      "_NET_WM_NAME" },
        { &g->wm_normal_hints,  "WM_NORMAL_HINTS" },
        { &g->clipboard,        "CLIPBOARD" },
        { &g->targets,          "TARGETS" },
        { &g->qprop,            "QUBES_SELECTION" },
        { &g->compound_text,    "COMPOUND_TEXT" },
        { &g->xembed,           "_XEMBED" },
    };
    Atom supported[SUPPORTED_ATOMS + QUBES_ARRAY_SIZE(atoms_to_intern)];
    /* pretend that GUI agent is window manager */
    const char *names[SUPPORTED_ATOMS + QUBES_ARRAY_SIZE(atoms_to_intern)] = {
        "_NET_SUPPORTED",
        "_NET_SUPPORTING_WM_CHECK",
        /* _NET_WM_MOVERESIZE required to disable broken GTK+ move/resize fallback */
        "_NET_WM_MOVERESIZE",
        "_NET_WM_STATE",
        "_NET_WM_STATE_FULLSCREEN",
        "_NET_WM_STATE_DEMANDS_ATTENTION",
    };
    for (size_t i = 0; i < QUBES_ARRAY_SIZE(atoms_to_intern); ++i)
        names[SUPPORTED_ATOMS + i] = atoms_to_intern[i].name;
    if (!XInternAtoms(g->display, (char **)names,
                      QUBES_ARRAY_SIZE(names), False, supported)) {
        fputs("Could not intern global atoms\n", stderr);
        exit(1);
    }
    for (size_t i = 0; i < QUBES_ARRAY_SIZE(atoms_to_intern); ++i)
        *atoms_to_intern[i].dest = supported[SUPPORTED_ATOMS + i];
    const Atom net_supported = supported[0], net_supporting_wm_check = supported[1];
    XChangeProperty(g->display, g->stub_win, g->net_wm_name, g->utf8_string_atom,
            8, PropModeReplace, (unsigned char*)"Qubes", 5);
    XChangeProperty(g->display, g->stub_win, net_supporting_wm_check, XA_WINDOW,
            32, PropModeReplace, (unsigned char*)&g->stub_win, 1);
    XChangeProperty(g->display, g->root_win, net_supporting_wm_check, XA_WINDOW,
            32, PropModeReplace, (unsigned char*)&g->stub_win, 1);
    XChangeProperty(g->display, g->root_win, net_supported, XA_ATOM,
            32, PropModeReplace, (unsigned char*)supported, SUPPORTED_ATOMS);

    g->clipboard_data = NULL;
    g->clipboard_data_len = 0;
}

static void handle_keypress(Ghandles * g, XID UNUSED(winid))
{
    struct msg_keypress key;
    XkbStateRec state;
    read_data(g->vchan, (char *) &key, sizeof(key));

    if(!g->created_input_device) {
        // sync modifiers state
        if (XkbGetState(g->display, XkbUseCoreKbd, &state) != Success) {
            if (g->log_level > 0)
                fprintf(stderr, "failed to get modifier state\n");
            state.mods = key.state;
        }
        if (!g->sync_all_modifiers) {
            // ignore all but CapsLock
            state.mods &= LockMask;
            key.state &= LockMask;
        }
        if (state.mods != key.state) {
            XModifierKeymap *modmap;
            int mod_index;
            int mod_mask;

            modmap = XGetModifierMapping(g->display);
            if (!modmap) {
                if (g->log_level > 0)
                    fprintf(stderr, "failed to get modifier mapping\n");
            } else {
                // from X.h:
                // #define ShiftMapIndex           0
                // #define LockMapIndex            1
                // #define ControlMapIndex         2
                // #define Mod1MapIndex            3
                // #define Mod2MapIndex            4
                // #define Mod3MapIndex            5
                // #define Mod4MapIndex            6
                // #define Mod5MapIndex            7
                for (mod_index = 0; mod_index < 8; mod_index++) {
                    if (modmap->modifiermap[mod_index*modmap->max_keypermod] == 0x00) {
                        if (g->log_level > 1)
                            fprintf(stderr, "ignoring disabled modifier %d\n", mod_index);
                        // no key set for this modifier, ignore
                        continue;
                    }
                    mod_mask = (1<<mod_index);
                    // special case for caps lock switch by press+release
                    if (mod_index == LockMapIndex) {
                        if ((state.mods & mod_mask) ^ (key.state & mod_mask)) {
                            feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 1);
                            feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 0);
                        }
                    } else {
                        if ((state.mods & mod_mask) && !(key.state & mod_mask))
                            feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 0);
                        else if (!(state.mods & mod_mask) && (key.state & mod_mask))
                            feed_xdriver(g, 'K', modmap->modifiermap[mod_index*modmap->max_keypermod], 1);
                    }
                }
                XFreeModifiermap(modmap);
            }
        }
        
        feed_xdriver(g, 'K', key.keycode, key.type == KeyPress ? 1 : 0);
    } else {
        int mod_mask;
        int mod_index;
        struct input_event iev;
        iev.type = EV_KEY;
        XModifierKeymap *modmap;
        modmap = XGetModifierMapping(g->display);
        
        if (!modmap) {
                if (g->log_level > 0)
                    fprintf(stderr, "failed to get modifier mapping\n");
        } else {
            for(mod_index = 0; mod_index < 8; mod_index++) {
                if (modmap->modifiermap[mod_index*modmap->max_keypermod] == 0x00) {
                        if (g->log_level > 1)
                            fprintf(stderr, "ignoring disabled modifier %d\n", mod_index);
                        // no key set for this modifier, ignore
                        continue;
                }
                mod_mask = (1<<mod_index);
                // special case for caps lock switch by press+release
                if (mod_index == LockMapIndex) {
                    if ((g->last_known_modifier_states & mod_mask) ^ (key.state & mod_mask)) {
                        iev.code = modmap->modifiermap[mod_index*modmap->max_keypermod] - 8;
                        iev.value = 1;
                        send_event(g, &iev);
                        iev.value = 0;
                        send_event(g, &iev);
                        // update state for caps_lock
                        g->last_known_modifier_states ^= mod_mask;
                    }
                } else {
                    // last modifier state was pressed down, modifier has since been released
                    if ((g->last_known_modifier_states & mod_mask) && !(key.state & mod_mask)) {
                        iev.code = modmap->modifiermap[mod_index*modmap->max_keypermod] - 8;
                        iev.value = 0;
                        // send modifier release
                        send_event(g, &iev);
                        // update state for this modifier
                        g->last_known_modifier_states ^= mod_mask;
                    }
                    
                    // last modifier state was up, modifier has since been pressed down
                    else if (!(g->last_known_modifier_states & mod_mask) && (key.state & mod_mask)) {
                        iev.code = modmap->modifiermap[mod_index*modmap->max_keypermod] - 8;
                        iev.value = 1;
                        // send modifier press
                        send_event(g, &iev);
                        // update state for this modifier
                        g->last_known_modifier_states ^= mod_mask;
                    }
                }
            }
        }
        
        XFreeModifiermap(modmap);

        // caps lock needs to be excluded to not send down, up, down or down, up, up on a caps lock sync instead of down, up
        if(key.keycode-8 != KEY_CAPSLOCK) {
            iev.code = key.keycode-8;
            iev.value = (key.type == KeyPress ? 1 : 0);
            send_event(g, &iev);
        }
        
    }
}

static void handle_button(Ghandles * g, XID winid)
{
    struct msg_button key;
    struct genlist *l = list_lookup(windows_list, winid);


    read_data(g->vchan, (char *) &key, sizeof(key));
    if (l && l->data && ((struct window_data*)l->data)->is_docked) {
        /* get position of embeder, not icon itself*/
        winid = ((struct window_data*)l->data)->embeder;
        XRaiseWindow(g->display, winid);
    }

    if (g->log_level > 1)
        fprintf(stderr,
                "send buttonevent, win 0x%lx type=%d button=%d\n",
                winid, key.type, key.button);
    feed_xdriver(g, 'B', key.button, key.type == ButtonPress ? 1 : 0);
}

static void handle_motion(Ghandles * g, XID winid)
{
    struct msg_motion key;
    //      XMotionEvent event;
    XWindowAttributes attr;
    int ret;
    struct genlist *l = list_lookup(windows_list, winid);

    read_data(g->vchan, (char *) &key, sizeof(key));
    if (l && l->data && ((struct window_data*)l->data)->is_docked) {
        /* get position of embeder, not icon itself*/
        winid = ((struct window_data*)l->data)->embeder;
    }
    ret = XGetWindowAttributes(g->display, winid, &attr);
    if (ret != 1) {
        fprintf(stderr,
                "XGetWindowAttributes for 0x%lx failed in "
                "do_button, ret=0x%x\n", winid, ret);
        return;
    }

    feed_xdriver(g, 'M', attr.x + key.x, attr.y + key.y);
}

// ensure that LeaveNotify is delivered to the window - if pointer is still
// above this window, place stub window between pointer and the window
static void handle_crossing(Ghandles * g, XID winid)
{
    struct msg_crossing key;
    XWindowAttributes attr;
    int ret;
    struct genlist *l = list_lookup(windows_list, winid);

    /* we want to always get root window child (as this we get from
     * XQueryPointer and can compare to window_under_pointer), so for embeded
     * window get the embeder */
    if (l && l->data && ((struct window_data*)l->data)->is_docked) {
        winid = ((struct window_data*)l->data)->embeder;
    }

    read_data(g->vchan, (char *) &key, sizeof(key));

    if (key.mode != NotifyNormal)
        return;

    if (key.type == EnterNotify) {
        ret = XGetWindowAttributes(g->display, winid, &attr);
        if (ret != 1) {
            fprintf(stderr,
                    "XGetWindowAttributes for 0x%lx failed in "
                    "handle_crossing, ret=0x%x\n", winid, ret);
            return;
        }

        // hide stub window
        XUnmapWindow(g->display, g->stub_win);
        feed_xdriver(g, 'M', attr.x + key.x, attr.y + key.y);
    } else if (key.type == LeaveNotify) {
        XID window_under_pointer, root_returned;
        int root_x, root_y, win_x, win_y;
        unsigned int mask_return;
        ret =
            XQueryPointer(g->display, g->root_win, &root_returned,
                    &window_under_pointer, &root_x, &root_y,
                    &win_x, &win_y, &mask_return);
        if (ret != 1) {
            fprintf(stderr,
                    "XQueryPointer for 0x%lx failed in "
                    "handle_crossing, ret=0x%x\n", winid,
                    ret);
            return;
        }
        // if pointer is still on the same window - place some stub window
        // just under it
        if (window_under_pointer == winid) {
            XMoveResizeWindow(g->display, g->stub_win,
                    root_x, root_y, 1, 1);
            XMapWindow(g->display, g->stub_win);
            XRaiseWindow(g->display, g->stub_win);
        }
    } else {
        fprintf(stderr, "Invalid crossing event: %d\n", key.type);
    }

}

static void take_focus(Ghandles * g, XID winid)
{
    // Send
    XClientMessageEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.display = g->display;
    ev.window = winid;
    ev.format = 32;
    ev.message_type = g->wmProtocols;
    ev.data.l[0] = g->wm_take_focus;
    ev.data.l[1] = g->time;
    XSendEvent(ev.display, ev.window, 1, 0, (XEvent *) & ev);
    if (g->log_level > 0)
        fprintf(stderr, "WM_TAKE_FOCUS sent for 0x%lx\n", winid);

}

static void handle_focus(Ghandles * g, XID winid)
{
    struct msg_focus key;
    struct genlist *l;
    int input_hint;
    int use_take_focus;

    read_data(g->vchan, (char *) &key, sizeof(key));
    if (key.type == FocusIn
            && (key.mode == NotifyNormal || key.mode == NotifyUngrab)) {

        XRaiseWindow(g->display, winid);

        if ( (l=list_lookup(windows_list, winid)) && (l->data) ) {
            input_hint = ((struct window_data*)l->data)->input_hint;
            use_take_focus = ((struct window_data*)l->data)->support_take_focus;
            if (((struct window_data*)l->data)->is_docked)
                XRaiseWindow(g->display, ((struct window_data*)l->data)->embeder);
        } else {
            fprintf(stderr, "WARNING handle_focus: FocusIn: Window 0x%lx data not initialized\n", winid);
            input_hint = True;
            use_take_focus = False;
        }

        // Give input focus only to window that set the input hint
        if (input_hint)
            XSetInputFocus(g->display, winid, RevertToParent, g->time);

        // Do not send take focus if the window doesn't support it
        if (use_take_focus)
            take_focus(g, winid);

        if (g->log_level > 1)
            fprintf(stderr, "0x%lx raised\n", winid);
    } else if (key.type == FocusOut
            && (key.mode == NotifyNormal
                || key.mode == NotifyUngrab)) {
        if ( (l=list_lookup(windows_list, winid)) && (l->data) )
            input_hint = ((struct window_data*)l->data)->input_hint;
        else {
            fprintf(stderr, "WARNING handle_focus: FocusOut: Window 0x%lx data not initialized\n", winid);
            input_hint = True;
        }
        if (input_hint)
            XSetInputFocus(g->display, None, RevertToParent, g->time);

        if (g->log_level > 1)
            fprintf(stderr, "0x%lx lost focus\n", winid);
    }

}

static int bitset(unsigned char *keys, int num)
{
    return (keys[num / 8] >> (num % 8)) & 1;
}

static void handle_keymap_notify(Ghandles * g)
{
    int i;
    unsigned char remote_keys[32], local_keys[32];
    read_struct(g->vchan, remote_keys);
    XQueryKeymap(g->display, (char *) local_keys);
    for (i = 0; i < 256; i++) {
        if (!bitset(remote_keys, i) && bitset(local_keys, i)) {
            feed_xdriver(g, 'K', i, 0);
            if (g->log_level > 1)
                fprintf(stderr,
                        "handle_keymap_notify: unsetting key %d\n",
                        i);
        }
    }
}


static void handle_configure(Ghandles * g, XID winid)
{
    struct msg_configure r;
    struct genlist *l = list_lookup(windows_list, winid);
    XWindowAttributes attr;
    XGetWindowAttributes(g->display, winid, &attr);
    read_data(g->vchan, (char *) &r, sizeof(r));
    if (l && l->data && ((struct window_data*)l->data)->is_docked) {
        XMoveResizeWindow(g->display, ((struct window_data*)l->data)->embeder, r.x, r.y, r.width, r.height);
        XMoveResizeWindow(g->display, winid, 0, 0, r.width, r.height);
    } else {
        XMoveResizeWindow(g->display, winid, r.x, r.y, r.width, r.height);
    }
    if (g->log_level > 0)
        fprintf(stderr,
                "configure msg, x/y %d %d (was %d %d), w/h %d %d (was %d %d)\n",
                r.x, r.y, attr.x, attr.y, r.width, r.height, attr.width,
                attr.height);

}

static void handle_map(Ghandles * g, XID winid)
{
    struct msg_map_info inf;
    XSetWindowAttributes attr;
    read_data(g->vchan, (char *) &inf, sizeof(inf));
    attr.override_redirect = inf.override_redirect;
    XChangeWindowAttributes(g->display, winid,
            CWOverrideRedirect, &attr);
    XMapWindow(g->display, winid);
    if (g->log_level > 1)
        fprintf(stderr, "map msg for 0x%lx\n", winid);
}

static void handle_close(Ghandles * g, XID winid)
{
    struct genlist *l;
    int use_delete_window;

    if ( (l=list_lookup(windows_list, winid)) && (l->data) ) {
        use_delete_window = ((struct window_data*)l->data)->support_delete_window;
    } else {
        fprintf(stderr, "WARNING handle_close: Window 0x%lx data not initialized\n",
                winid);
        use_delete_window = True; /* gentler, though it may be a no-op */
    }

    if (use_delete_window) {
        XClientMessageEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.display = g->display;
        ev.window = winid;
        ev.format = 32;
        ev.message_type = g->wmProtocols;
        ev.data.l[0] = g->wmDeleteWindow;
        XSendEvent(ev.display, ev.window, 1, 0, (XEvent *) & ev);
        if (g->log_level > 0)
            fprintf(stderr, "wmDeleteWindow sent for 0x%lx\n", winid);
    } else {
        XKillClient(g->display, winid);
        if (g->log_level > 0)
            fprintf(stderr, "XKillClient() called for 0x%lx\n", winid);
    }
}

/* start X server, returns its PID
 */
static pid_t do_execute_xorg(
        char *w_str, char *h_str, char *mem_str, char *depth_str,
        char *gui_domid_str)
{
    pid_t pid;
    int fd;

    pid = fork();
    switch (pid) {
        case -1:
            perror("fork");
            return -1;
        case 0:
            setenv("W", w_str, 1);
            setenv("H", h_str, 1);
            setenv("MEM", mem_str, 1);
            setenv("DEPTH", depth_str, 1);
            setenv("GUI_DOMID", gui_domid_str, 1);
            /* don't leak other FDs */
            for (fd = 3; fd < 256; fd++)
                close(fd);
            execl("/usr/bin/qubes-run-xorg", "qubes-run-xorg", NULL);
            perror("execl cmd");
            exit(127);
        default:
            return pid;
    }
}

static void terminate_and_cleanup_xorg(Ghandles *g) {
    int status;

    if (g->x_pid != (pid_t)-1) {
        kill(g->x_pid, SIGTERM);
        waitpid(g->x_pid, &status, 0);
        g->x_pid = -1;
    }
}

#define CLIPBOARD_4WAY
static void handle_clipboard_req(Ghandles * g, XID winid)
{
    Atom Clp;
    Window owner;
#ifdef CLIPBOARD_4WAY
    Clp = g->clipboard;
#else
    Clp = XA_PRIMARY;
#endif
    owner = XGetSelectionOwner(g->display, Clp);
    if (g->log_level > 0)
        fprintf(stderr, "clipboard req, owner=0x%lx\n", owner);
    if (owner == None) {
        send_clipboard_data(g->vchan, winid, NULL, 0, g->protocol_version);
        return;
    }
    XConvertSelection(g->display, Clp, g->targets, g->qprop, g->stub_win, g->time);
}

static void handle_clipboard_data(Ghandles * g, XID UNUSED(winid),
        unsigned int len)
{
    if (g->clipboard_data)
        free(g->clipboard_data);
    // qubes_guid will not bother to send len==-1, really
    g->clipboard_data = malloc(len + 1);
    if (!g->clipboard_data) {
        perror("malloc");
        exit(1);
    }
    g->clipboard_data_len = len;
    read_data(g->vchan, (char *) g->clipboard_data, len);
    g->clipboard_data[len] = 0;
    g->clipboard_last_access = g->time;
    XSetSelectionOwner(g->display, XA_PRIMARY, g->stub_win, g->time);
    XSetSelectionOwner(g->display, g->clipboard, g->stub_win, g->time);
#ifndef CLIPBOARD_4WAY
    XSync(g->display, False);
    feed_xdriver(g, 'B', 2, 1);
    feed_xdriver(g, 'B', 2, 0);
#endif
}

static void handle_window_flags(Ghandles *g, XID winid)
{
    int ret, j, changed;
    unsigned i;
    Atom *state_list;
    Atom new_state_list[12];
    Atom act_type;
    int act_fmt;
    uint32_t tmp_flag;
    unsigned long nitems, bytesleft;
    struct msg_window_flags msg_flags;
    read_data(g->vchan, (char *) &msg_flags, sizeof(msg_flags));

    /* FIXME: only first 10 elements are parsed */
    ret = XGetWindowProperty(g->display, winid, g->net_wm_state, 0, 10,
            False, XA_ATOM, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&state_list);
    if (ret != Success)
        return;

    j = 0;
    changed = 0;
    for (i=0; i < nitems; i++) {
        tmp_flag = flags_from_atom(g, state_list[i]);
        if (tmp_flag && tmp_flag & msg_flags.flags_set) {
            /* leave flag set, mark as processed */
            msg_flags.flags_set &= ~tmp_flag;
        } else if (tmp_flag && tmp_flag & msg_flags.flags_unset) {
            /* skip this flag (remove) */
            changed = 1;
            continue;
        }
        /* copy flag to new set */
        new_state_list[j++] = state_list[i];
    }
    XFree(state_list);
    /* set new elements */
    if (msg_flags.flags_set & WINDOW_FLAG_FULLSCREEN)
        new_state_list[j++] = g->wm_state_fullscreen;
    if (msg_flags.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION)
        new_state_list[j++] = g->wm_state_demands_attention;

    if (msg_flags.flags_set)
        changed = 1;

    if (!changed)
        return;

    XChangeProperty(g->display, winid, g->net_wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)new_state_list, j);
}

static void handle_message(Ghandles * g)
{
    struct msg_hdr hdr;
    char discard[256];
    read_data(g->vchan, (char *) &hdr, sizeof(hdr));
    if (g->log_level > 1)
        fprintf(stderr, "received message type %d for 0x%x\n", hdr.type, hdr.window);
    switch (hdr.type) {
        case MSG_KEYPRESS:
            handle_keypress(g, hdr.window);
            break;
        case MSG_CONFIGURE:
            handle_configure(g, hdr.window);
            break;
        case MSG_MAP:
            handle_map(g, hdr.window);
            break;
        case MSG_BUTTON:
            handle_button(g, hdr.window);
            break;
        case MSG_MOTION:
            handle_motion(g, hdr.window);
            break;
        case MSG_CLOSE:
            handle_close(g, hdr.window);
            break;
        case MSG_CROSSING:
            handle_crossing(g, hdr.window);
            break;
        case MSG_FOCUS:
            handle_focus(g, hdr.window);
            break;
        case MSG_CLIPBOARD_REQ:
            handle_clipboard_req(g, hdr.window);
            break;
        case MSG_CLIPBOARD_DATA:
            handle_clipboard_data(g, hdr.window, hdr.untrusted_len);
            break;
        case MSG_KEYMAP_NOTIFY:
            handle_keymap_notify(g);
            break;
        case MSG_WINDOW_FLAGS:
            handle_window_flags(g, hdr.window);
            break;
        case MSG_DESTROY:
            /* currently not used */
            break;
        case MSG_WINDOW_DUMP_ACK:
            if (g->protocol_version >= QUBES_GUID_MIN_MSG_WINDOW_DUMP_ACK) {
                feed_xdriver(g, 'a', 0, 0);
                break;
            }
            __attribute__((fallthrough));
        default:
            fprintf(stderr, "got unknown msg type %d, ignoring\n", hdr.type);
            while (hdr.untrusted_len > 0) {
#define min(x,y) ((x)>(y)?(y):(x))
                hdr.untrusted_len -= read_data(g->vchan, discard, min(hdr.untrusted_len, sizeof(discard)));
#undef min
            }
    }
}

static pid_t get_xconf_and_run_x(Ghandles *g)
{
    struct msg_xconf xconf;
    char w_str[12], h_str[12], mem_str[12], depth_str[12], gui_domid_str[12];
    pid_t x_pid;
    read_struct(g->vchan, xconf);
    snprintf(w_str, sizeof(w_str), "%d", xconf.w);
    snprintf(h_str, sizeof(h_str), "%d", xconf.h);
    snprintf(mem_str, sizeof(mem_str), "%d", xconf.mem);
    snprintf(depth_str, sizeof(depth_str), "%d", xconf.depth);
    snprintf(gui_domid_str, sizeof(gui_domid_str), "%u", g->domid);
    x_pid = do_execute_xorg(w_str, h_str, mem_str, depth_str, gui_domid_str);
    if (x_pid == (pid_t)-1) {
        errx(1, "X server startup failed");
    }
    return x_pid;
}

static void handshake(Ghandles *g)
{
    uint32_t version = PROTOCOL_VERSION;
    write_struct(g->vchan, version);
    version = 0;
    read_struct(g->vchan, version);
    uint16_t major_version = version >> 16, minor_version = version & 0xFFFF;
    if (version > PROTOCOL_VERSION ||
        major_version != PROTOCOL_VERSION_MAJOR ||
        minor_version < 4)
        errx(2, "Incompatible GUI daemon version (offered %" PRIu16 ".%" PRIu16
                ", got %" PRIu16 ".%" PRIu16 ")",
                PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR,
                major_version, minor_version);
    g->protocol_version = version;
}

static void handle_guid_disconnect(void)
{
    Ghandles *g = ghandles_for_vchan_reinitialize;
    struct msg_xconf xconf;

    if (!ghandles_for_vchan_reinitialize) {
        fprintf(stderr, "gui-daemon disconnected before fully initialized, "
                "cannot reconnect, exiting!\n");
        exit(1);
    }
    libvchan_close(g->vchan);
    g->vchan = libvchan_server_init(g->domid, 6000, 4096, 4096);
    /* wait for gui daemon */
    while (libvchan_is_open(g->vchan) == VCHAN_WAITING)
        libvchan_wait(g->vchan);
    handshake(g);
    /* discard */
    read_struct(g->vchan, xconf);
    send_all_windows_info(g);
}

static _Noreturn void handle_sigterm(int UNUSED(sig),
        siginfo_t *UNUSED(info), void *UNUSED(context))
{
    Ghandles *g = ghandles_for_vchan_reinitialize;
    terminate_and_cleanup_xorg(g);
    exit(0);
}

static void handle_sigchld(int UNUSED(sig),
        siginfo_t *UNUSED(info), void *UNUSED(context))
{
    Ghandles *g = ghandles_for_vchan_reinitialize;
    if (g->x_pid != (pid_t)-1) {
        int status;
        pid_t pid = waitpid(g->x_pid, &status, WNOHANG);
        if (pid == g->x_pid && (WIFEXITED(status) || WIFSIGNALED(status)))
            /* TODO: consider saving also exit status, but right now gui-agent
             * would handle it the same regardless, so maybe later, just for
             * logging purposes */
            g->x_pid = -1;
    }
}

static void usage(void)
{
    fprintf(stderr, "Usage: qubes_gui [options]\n");
    fprintf(stderr, "       -v  increase log verbosity\n");
    fprintf(stderr, "       -q  decrease log verbosity\n");
    fprintf(stderr, "       -m  sync all modifiers before key event (default)\n");
    fprintf(stderr, "       -M  sync only Caps Lock key event\n");
    fprintf(stderr, "       -c  turn off composite \"redirect automatic\" mode\n");
    fprintf(stderr, "       -h  print this message\n");
    fprintf(stderr, "       -d  GUI domain id (default: 0)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Log levels:\n");
    fprintf(stderr, " 0 - only errors\n");
    fprintf(stderr, " 1 - some basic messages (default)\n");
    fprintf(stderr, " 2 - debug\n");
}

static void parse_args(Ghandles * g, int argc, char **argv)
{
    int opt;

    // defaults
    g->log_level = 0;
    g->sync_all_modifiers = 1;
    g->composite_redirect_automatic = 1;
    g->domid = 0;
    while ((opt = getopt(argc, argv, "qvchmMd:")) != -1) {
        switch (opt) {
            case 'q':
                g->log_level--;
                break;
            case 'v':
                g->log_level++;
                break;
            case 'm':
                g->sync_all_modifiers = 1;
                break;
            case 'M':
                g->sync_all_modifiers = 0;
                break;
            case 'c':
                g->composite_redirect_automatic = 0;
                break;
            case 'h':
                usage();
                exit(0);
            case 'd':
                g->domid = atoi(optarg);
                break;
            default:
                usage();
                exit(1);
        }
    }

    if (g->log_level >= 2)
        print_x11_errors = 1;
}

int main(int argc, char **argv)
{
    int i;
    int xfd;
    Ghandles g = { .x_pid = -1 };

    g.created_input_device = access("/run/qubes-service/gui-agent-virtual-input-device", F_OK) == 0;

    if(g.created_input_device) {
        // open uinput, if it fails, falls back to xdriver to not break input to the qube
        g.uinput_fd = open("/dev/uinput", O_WRONLY | O_NDELAY);
        if(g.uinput_fd < 0) {
            g.uinput_fd = open("/dev/input/uinput", O_WRONLY | O_NDELAY);
            
            if(g.uinput_fd < 0) {
                fprintf(stderr, "Couldn't open uinput, falling back to xdriver\n");
                g.created_input_device = 0;
            }
        }
    }
    
    // input device creation
    if(g.created_input_device) {
        
        if (ioctl(g.uinput_fd, UI_SET_EVBIT, EV_SYN) < 0) {
            fprintf(stderr, "error setting EVBIT for EV_SYN, falling back to xdriver\n");
            g.created_input_device = 0;
        }

        if (ioctl(g.uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
            fprintf(stderr, "error setting EVBIT for EV_KEY, falling back to xdriver\n");
            g.created_input_device = 0;
        }

        // set all keys
        for(int i = 1; i < KEY_MAX; i++) {
            if((i < BTN_MISC || i > BTN_GEAR_UP) && i != KEY_RESERVED && ioctl(g.uinput_fd, UI_SET_KEYBIT, i) < 0) {
                fprintf(stderr, "Not able to set KEYBIT %d\n", i);
            }
        }
        
        struct uinput_setup usetup;
        memset(&usetup, 0, sizeof(usetup));
        strcpy(usetup.name, "Qubes Virtual Input Device");
        
        if(ioctl(g.uinput_fd, UI_DEV_SETUP, &usetup) < 0) {
            fprintf(stderr, "Input device setup failed, falling back to xdriver\n");
            g.created_input_device = 0;
        } else {
            
            if(ioctl(g.uinput_fd, UI_DEV_CREATE) < 0) {
                fprintf(stderr, "Input device creation failed, falling back to xdriver\n");
                g.created_input_device = 0;
            }
        }
        
        g.last_known_modifier_states = 0;
    }


    parse_args(&g, argc, argv);

    /* Clipboard wipe functionality is controlled by the
     * gui-agent-clipboard-wipe service
     */
    g.clipboard_wipe =
        access("/run/qubes-service/gui-agent-clipboard-wipe", F_OK) == 0;

    g.vchan = libvchan_server_init(g.domid, 6000, 4096, 4096);
    if (!g.vchan) {
        fprintf(stderr, "vchan initialization failed\n");
        exit(1);
    }
    /* wait for gui daemon */
    while (libvchan_is_open(g.vchan) == VCHAN_WAITING)
        libvchan_wait(g.vchan);
    saved_argv = argv;
    vchan_register_at_eof(handle_guid_disconnect);

    ghandles_for_vchan_reinitialize = &g;
    struct sigaction sigchld_handler = {
        .sa_sigaction = handle_sigchld,
        .sa_flags = SA_SIGINFO,
    };
    sigemptyset(&sigchld_handler.sa_mask);
    if (sigaction(SIGCHLD, &sigchld_handler, NULL))
        err(1, "sigaction");
    struct sigaction sigterm_handler = {
        .sa_sigaction = handle_sigterm,
        .sa_flags = SA_SIGINFO,
    };
    sigemptyset(&sigterm_handler.sa_mask);
    if (sigaction(SIGTERM, &sigterm_handler, NULL))
        err(1, "sigaction");

    handshake(&g);
    g.x_pid = get_xconf_and_run_x(&g);

    mkghandles(&g);
    /* Turn on Composite for all children of root window. This way X server
     * keeps separate buffers for each (root child) window.
     * There are two modes:
     *  - manual - this way only off-screen buffers are maintained
     *  - automatic - in addition to manual, widows are rendered back to the
     *  root window
     */
    for (i = 0; i < ScreenCount(g.display); i++)
        XCompositeRedirectSubwindows(g.display,
                RootWindow(g.display, i),
            (g.composite_redirect_automatic ?
              CompositeRedirectAutomatic :
              CompositeRedirectManual));
    for (i = 0; i < ScreenCount(g.display); i++)
        XSelectInput(g.display, RootWindow(g.display, i),
                SubstructureNotifyMask);


    if (!XDamageQueryExtension(g.display, &damage_event,
                &damage_error)) {
        perror("XDamageQueryExtension");
        exit(1);
    }

    /* Sort the cursors table for use with bsearch. */
    qsort(supported_cursors,
          NUM_SUPPORTED_CURSORS, sizeof(supported_cursors[0]),
          compare_supported_cursors);

    /* Use XFixes to handle cursor shape. */
    if (XFixesQueryExtension(
            g.display, &xfixes_event, &xfixes_error)) {
        for (i = 0; i < ScreenCount(g.display); i++)
            XFixesSelectCursorInput(g.display, RootWindow(g.display, i),
                                    XFixesDisplayCursorNotifyMask);
    } else
        fprintf(stderr, "XFixes not available, cursor shape handling off\n");

    XAutoRepeatOff(g.display);
    windows_list = list_new();
    embeder_list = list_new();
    XSetErrorHandler(dummy_handler);
    XSetSelectionOwner(g.display, g.tray_selection,
            g.stub_win, CurrentTime);
    if (XGetSelectionOwner(g.display, g.tray_selection) ==
            g.stub_win) {
        XClientMessageEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.send_event = True;
        if ((ev.message_type = XInternAtom(g.display, "MANAGER", False)) == None) {
            fputs("Cannot intern MANAGER atom\n", stderr);
            exit(1);
        }
        ev.window = DefaultRootWindow(g.display);
        ev.format = 32;
        ev.data.l[0] = CurrentTime;
        ev.data.l[1] = g.tray_selection;
        ev.data.l[2] = g.stub_win;
        ev.display = g.display;
        XSendEvent(ev.display, ev.window, False, NoEventMask,
                (XEvent *) & ev);
        if (g.log_level > 0)
            fprintf(stderr,
                    "Acquired MANAGER selection for tray\n");
    }
    xfd = ConnectionNumber(g.display);
    struct pollfd fds[] = {
        { .fd = -1, .events = POLLIN | POLLHUP, .revents = 0 },
        { .fd = xfd, .events = POLLIN | POLLHUP, .revents = 0 },
        { .fd = g.xserver_fd, .events = POLLIN | POLLHUP, .revents = 0 },
    };
    for (;;) {
        int busy;

        if (g.x_pid == -1) {
            fprintf(stderr, "Xorg exited prematurely\n");
            exit(1);
        }

        fds[0].fd = libvchan_fd_for_select(g.vchan);
        wait_for_vchan_or_argfd(g.vchan, fds, QUBES_ARRAY_SIZE(fds));
        /* first process possible qubes_drv reconnection, otherwise we may be
         * using stale g.xserver_fd */
        if (fds[2].revents) {
            char discard[64];
            int ret;

            /* unexpected data from qubes_drv, check for possible EOF */
            ret = read(g.xserver_fd, discard, sizeof(discard));
            if (ret > 0) {
                fprintf(stderr,
                        "Got unexpected %d bytes from qubes_drv, something is wrong\n",
                        ret);
                exit(1);
            } else if (ret == 0) {
                fprintf(stderr,
                        "qubes_drv disconnected, waiting for possible reconnection\n");
                close(g.xserver_fd);
                wait_for_unix_socket(&g);
                fds[2].fd = g.xserver_fd;
            } else {
                perror("reading from qubes_drv");
                exit(1);
            }
        }

        do {
            busy = 0;
            if (XPending(g.display)) {
                process_xevent(&g);
                busy = 1;
            }
            while (libvchan_data_ready(g.vchan)) {
                handle_message(&g);
                busy = 1;
            }
        } while (busy);

    }
    return 0;
}

/* vim: set sw=4 ts=4 sts=4 et eol ff=unix fenc=utf-8: */
