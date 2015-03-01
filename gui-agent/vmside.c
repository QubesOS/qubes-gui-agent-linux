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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/XKBlib.h>
#include <X11/Xlibint.h>
#include <X11/Xatom.h>
#include <qubes-gui-protocol.h>
#include <qubes-xorg-tray-defs.h>
#include "xdriver-shm-cmd.h"
#include "txrx.h"
#include "list.h"
#include "error.h"
#include <libvchan.h>

#define QUBES_GUI_PROTOCOL_VERSION_LINUX (1 << 16 | 0)

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

int damage_event, damage_error;

char **saved_argv;

struct _global_handles {
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	GC context;
	Atom wmDeleteMessage;
	Atom wmProtocols;
	Atom tray_selection;	/* Atom: _NET_SYSTEM_TRAY_SELECTION_S<creen number> */
	Atom tray_opcode;	/* Atom: _NET_SYSTEM_TRAY_MESSAGE_OPCODE */
	Atom xembed_info;	/* Atom: _XEMBED_INFO */
	Atom utf8_string_atom; /* Atom: UTF8_STRING */
	Atom wm_state;         /* Atom: _NET_WM_STATE */
	Atom wm_state_fullscreen; /* Atom: _NET_WM_STATE_FULLSCREEN */
	Atom wm_state_demands_attention; /* Atom: _NET_WM_STATE_DEMANDS_ATTENTION */
	Atom wm_take_focus;	/* Atom: WM_TAKE_FOCUS */
	int xserver_fd;
	libvchan_t *vchan;
	Window stub_win;    /* window for clipboard operations and to simulate LeaveNotify events */
	unsigned char *clipboard_data;
	unsigned int clipboard_data_len;
	int log_level;
	int sync_all_modifiers;
};

struct window_data {
	int is_docked; /* is it docked icon window */
	XID embeder;   /* for docked icon points embeder window */
	int input_hint; /* the window should get input focus - False=Never */
	int support_take_focus;
};

struct embeder_data {
	XID icon_window;
};

struct genlist *windows_list;
struct genlist *embeder_list;
typedef struct _global_handles Ghandles;
Ghandles *ghandles_for_vchan_reinitialize;

#define SKIP_NONMANAGED_WINDOW if (!list_lookup(windows_list, window)) return

void send_wmnormalhints(Ghandles * g, XID window, int ignore_fail);
void retrieve_wmhints(Ghandles * g, XID window, int ignore_fail);
void retrieve_wmprotocols(Ghandles * g, XID window, int ignore_fail);

void process_xevent_damage(Ghandles * g, XID window,
			   int x, int y, int width, int height)
{
	struct msg_shmimage mx;
	struct msg_hdr hdr;
	SKIP_NONMANAGED_WINDOW;

	hdr.type = MSG_SHMIMAGE;
	hdr.window = window;
	mx.x = x;
	mx.y = y;
	mx.width = width;
	mx.height = height;
	write_message(g->vchan, hdr, mx);
}

void process_xevent_createnotify(Ghandles * g, XCreateWindowEvent * ev)
{
	struct msg_hdr hdr;
	struct msg_create crt;
	struct window_data *wd;

	XWindowAttributes attr;
	int ret;
	ret = XGetWindowAttributes(g->display, ev->window, &attr);
	if (ret != 1) {
		fprintf(stderr, "XGetWindowAttributes for 0x%x failed in "
			"handle_create, ret=0x%x\n", (int) ev->window,
			ret);
		return;
	};

	if (g->log_level > 0)
		fprintf(stderr, "Create for 0x%x class 0x%x\n",
			(int) ev->window, attr.class);
	if (list_lookup(windows_list, ev->window)) {
		fprintf(stderr, "CREATE for already existing 0x%x\n",
			(int) ev->window);
		return;
	}
	if (list_lookup(embeder_list, ev->window)) {
		/* ignore CreateNotify for embeder window */
		if (g->log_level > 1)
			fprintf(stderr, "CREATE for embeder 0x%x\n",
					(int) ev->window);
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
	wd->support_take_focus = False;
	list_insert(windows_list, ev->window, wd);

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
	retrieve_wmprotocols(g, hdr.window, 1);
	retrieve_wmhints(g, hdr.window, 1);
}

void feed_xdriver(Ghandles * g, int type, int arg1, int arg2)
{
	char ans;
	int ret;
	struct xdriver_cmd cmd;

	cmd.type = type;
	cmd.arg1 = arg1;
	cmd.arg2 = arg2;
	if (write(g->xserver_fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
		perror("unix write");
		exit(1);
	}
	ans = '1';
	ret = read(g->xserver_fd, &ans, 1);
	if (ret != 1 || ans != '0') {
		perror("unix read");
		fprintf(stderr, "read returned %d, char read=0x%x\n", ret,
			(int) ans);
		exit(1);
	}
}

void read_discarding(int fd, int size)
{
	char buf[1024];
	int n, count, total = 0;
	while (total < size) {
		if (size > (int)sizeof(buf))
			count = sizeof(buf);
		else
			count = size;
		n = read(fd, buf, count);
		if (n < 0) {
			perror("read_discarding");
			exit(1);
		}
		if (n == 0) {
			fprintf(stderr, "EOF in read_discarding\n");
			exit(1);
		}
		total += n;
	}
}

void send_pixmap_mfns(Ghandles * g, XID window)
{
	struct shm_cmd shmcmd;
	struct msg_hdr hdr;
	uint32_t *mfnbuf;
	int ret, rcvd = 0, size;

	feed_xdriver(g, 'W', (int) window, 0);
	if (read(g->xserver_fd, &shmcmd, sizeof(shmcmd)) != sizeof(shmcmd)) {
		perror("unix read shmcmd");
		exit(1);
	}
	if (shmcmd.num_mfn == 0 || shmcmd.num_mfn > (unsigned)MAX_MFN_COUNT ||
			shmcmd.width > MAX_WINDOW_WIDTH || shmcmd.height > MAX_WINDOW_HEIGHT) {
		fprintf(stderr, "got num_mfn=0x%x for window 0x%x (%dx%d)\n",
			shmcmd.num_mfn, (int) window, shmcmd.width, shmcmd.height);
		read_discarding(g->xserver_fd,
				shmcmd.num_mfn * sizeof(*mfnbuf));
		return;
	}
	size = shmcmd.num_mfn * sizeof(*mfnbuf);
	mfnbuf = alloca(size);
	while (rcvd < size) {
		ret =
		    read(g->xserver_fd, ((char *) mfnbuf) + rcvd,
			 size - rcvd);
		if (ret == 0) {
			fprintf(stderr, "unix read EOF\n");
			exit(1);
		}
		if (ret < 0) {
			perror("unix read error");
			exit(1);
		}
		rcvd += ret;
	}
	hdr.type = MSG_MFNDUMP;
	hdr.window = window;
	hdr.untrusted_len = sizeof(shmcmd) + size;
	write_struct(g->vchan, hdr);
	write_struct(g->vchan, shmcmd);
	write_data(g->vchan, (char *) mfnbuf, size);
}

void getwmname_tochar(Ghandles * g, XID window, char *outbuf, int bufsize)
{
	XTextProperty text_prop_return;
	char **list;
	int count;

	outbuf[0] = 0;
	if (!XGetWMName(g->display, window, &text_prop_return) ||
	    !text_prop_return.value || !text_prop_return.nitems)
		return;
	if (Xutf8TextPropertyToTextList(g->display,
					&text_prop_return, &list,
					&count) < 0 || count <= 0
	    || !*list) {
		XFree(text_prop_return.value);
		return;
	}
	strncat(outbuf, list[0], bufsize);
	XFree(text_prop_return.value);
	XFreeStringList(list);
	if (g->log_level > 0)
		fprintf(stderr, "got wmname=%s\n", outbuf);
}

void send_wmname(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	struct msg_wmname msg;
	memset(&msg, 0, sizeof(msg));
	getwmname_tochar(g, window, msg.data, sizeof(msg.data));
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

	if (XGetWMProtocols(g->display, window, &supported_protocols, &nitems) == 1) {
		for (i=0; i < nitems; i++) {
			if (supported_protocols[i] == g->wm_take_focus) {
				// Retrieve window data and set support_take_focus
				if (!((l=list_lookup(windows_list, window)) && (l->data))) {
					fprintf(stderr, "ERROR retrieve_wmprotocols: Window 0x%x data not initialized", (int)window);
					return;
				}
				if (g->log_level > 1)
					fprintf(stderr, "Protocol take_focus supported for Window 0x%x\n", (int)window);

				((struct window_data*)l->data)->support_take_focus = True;
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
		fprintf(stderr, "ERROR retrieve_wmhints: Window 0x%x data not initialized", (int)window);
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
			fprintf(stderr, "Received input hint 0x%x for Window 0x%x\n", wm_hints->input, (int)window);
	} else {
		// Default value
		if (g->log_level > 1)
			fprintf(stderr, "Received WMHints without input hint set for Window 0x%x\n", (int)window);
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

void send_window_state(Ghandles * g, XID window)
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
	ret = XGetWindowProperty(g->display, window, g->wm_state, 0, 10,
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

void process_xevent_map(Ghandles * g, XID window)
{
	XWindowAttributes attr;
	struct msg_hdr hdr;
	struct msg_map_info map_info;
	Window transient;
	SKIP_NONMANAGED_WINDOW;

	if (g->log_level > 1)
		fprintf(stderr, "MAP for window 0x%x\n", (int)window);
	send_pixmap_mfns(g, window);
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
//      process_xevent_damage(g, window, 0, 0, attr.width, attr.height);
}

void process_xevent_unmap(Ghandles * g, XID window)
{
	struct msg_hdr hdr;
	SKIP_NONMANAGED_WINDOW;

	if (g->log_level > 1)
		fprintf(stderr, "UNMAP for window 0x%x\n", (int)window);
	hdr.type = MSG_UNMAP;
	hdr.window = window;
	hdr.untrusted_len = 0;
	write_struct(g->vchan, hdr);
	XDeleteProperty(g->display, window, g->wm_state);
}

void process_xevent_destroy(Ghandles * g, XID window)
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
		fprintf(stderr, "handle destroy 0x%x\n", (int) window);
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

void process_xevent_configure(Ghandles * g, XID window,
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
			"handle configure event 0x%x w=%d h=%d ovr=%d\n",
			(int) window, ev->width, ev->height,
			(int) ev->override_redirect);
	if (l && l->data && ((struct window_data*)l->data)->is_docked) {
		/* for docked icon, ensure that it fills embeder window; don't send any
		 * message to dom0 - it will be done for embeder itself*/
		XWindowAttributes attr;
		int ret;

		ret = XGetWindowAttributes(g->display, ((struct window_data*)l->data)->embeder, &attr);
		if (ret != 1) {
			fprintf(stderr,
					"XGetWindowAttributes for 0x%x failed in "
					"handle_xevent_configure, ret=0x%x\n", (int) ((struct window_data*)l->data)->embeder, ret);
			return;
		};
		if (ev->x != 0 || ev->y != 0 || ev->width != attr.width || ev->height != attr.height) {
			XMoveResizeWindow(g->display, window, 0, 0, attr.width, attr.height);
		}
		return;
	}
	hdr.type = MSG_CONFIGURE;
	hdr.window = window;
	conf.x = ev->x;
	conf.y = ev->y;
	conf.width = ev->width;
	conf.height = ev->height;
	conf.override_redirect = ev->override_redirect;
	write_message(g->vchan, hdr, conf);
	send_pixmap_mfns(g, window);
}

void send_clipboard_data(libvchan_t *vchan, char *data, int len)
{
	struct msg_hdr hdr;
	hdr.type = MSG_CLIPBOARD_DATA;
	if (len > MAX_CLIPBOARD_SIZE)
		len = MAX_CLIPBOARD_SIZE;
	else
		hdr.window = len;
	hdr.window = len;
	hdr.untrusted_len = len;
	write_struct(vchan, hdr);
	write_data(vchan, (char *) data, len);
}

void handle_targets_list(Ghandles * g, Atom Qprop, unsigned char *data,
			 int len)
{
	Atom Clp = XInternAtom(g->display, "CLIPBOARD", False);
	Atom *atoms = (Atom *) data;
	int i;
	int have_utf8 = 0;
	if (g->log_level > 1)
		fprintf(stderr, "target list data size %d\n", len);
	for (i = 0; i < len; i++) {
		if (atoms[i] == g->utf8_string_atom)
			have_utf8 = 1;
		if (g->log_level > 1)
			fprintf(stderr, "supported 0x%x %s\n",
				(int) atoms[i], XGetAtomName(g->display,
							     atoms[i]));
	}
	XConvertSelection(g->display, Clp,
			  have_utf8 ? g->utf8_string_atom : XA_STRING, Qprop,
			  g->stub_win, CurrentTime);
}


void process_xevent_selection(Ghandles * g, XSelectionEvent * ev)
{
	int format, result;
	Atom type;
	unsigned long len, bytes_left, dummy;
	unsigned char *data;
	Atom Clp = XInternAtom(g->display, "CLIPBOARD", False);
	Atom Qprop = XInternAtom(g->display, "QUBES_SELECTION", False);
	Atom Targets = XInternAtom(g->display, "TARGETS", False);
	Atom Utf8_string_atom =
	    XInternAtom(g->display, "UTF8_STRING", False);

	if (g->log_level > 0)
		fprintf(stderr, "selection event, target=%s\n",
			XGetAtomName(g->display, ev->target));
	if (ev->requestor != g->stub_win || ev->property != Qprop)
		return;
	XGetWindowProperty(g->display, ev->requestor, Qprop, 0, 0, 0,
			   AnyPropertyType, &type, &format, &len,
			   &bytes_left, &data);
	if (bytes_left <= 0)
		return;
	result =
	    XGetWindowProperty(g->display, ev->requestor, Qprop, 0,
			       bytes_left, 0,
			       AnyPropertyType, &type,
			       &format, &len, &dummy, &data);
	if (result != Success)
		return;

	if (ev->target == Targets)
		handle_targets_list(g, Qprop, data, len);
	// If we receive TARGETS atom in response for TARGETS query, let's assume
	// that UTF8 is supported.
	// this is workaround for Opera web browser...
	else if (ev->target == XA_ATOM && len >= 4 && len <= 8 &&
		 // compare only first 4 bytes
		 *((unsigned *) data) == Targets)
		XConvertSelection(g->display, Clp,
				  Utf8_string_atom, Qprop,
				  g->stub_win, CurrentTime);
	else
		send_clipboard_data(g->vchan, (char *) data, len);
	/* even if the clipboard owner does not support UTF8 and we requested
	   XA_STRING, it is fine - ascii is legal UTF8 */
	XFree(data);

}

void process_xevent_selection_req(Ghandles * g,
				  XSelectionRequestEvent * req)
{
	XSelectionEvent resp;
	Atom Targets = XInternAtom(g->display, "TARGETS", False);
	Atom Compound_text =
	    XInternAtom(g->display, "COMPOUND_TEXT", False);
	Atom Utf8_string_atom =
	    XInternAtom(g->display, "UTF8_STRING", False);
	int convert_style = XConverterNotFound;

	if (g->log_level > 0)
		fprintf(stderr, "selection req event, target=%s\n",
			XGetAtomName(g->display, req->target));
	resp.property = None;
	if (req->target == Targets) {
		Atom tmp[4] = { XA_STRING, Targets, Utf8_string_atom,
			Compound_text
		};
		XChangeProperty(g->display, req->requestor, req->property,
				XA_ATOM, 32, PropModeReplace,
				(unsigned char *)
				tmp, sizeof(tmp) / sizeof(tmp[0]));
		resp.property = req->property;
	}
	if (req->target == Utf8_string_atom)
		convert_style = XUTF8StringStyle;
	if (req->target == XA_STRING)
		convert_style = XTextStyle;
	if (req->target == Compound_text)
		convert_style = XCompoundTextStyle;
	if (convert_style != XConverterNotFound) {
		XTextProperty ct;
		Xutf8TextListToTextProperty(g->display,
					    (char **) &g->clipboard_data,
					    1, convert_style, &ct);
		XSetTextProperty(g->display, req->requestor, &ct,
				 req->property);
		XFree(ct.value);
		resp.property = req->property;
	}

	if (resp.property == None)
		fprintf(stderr,
			"Not supported selection_req target 0x%x %s\n",
			(int) req->target, XGetAtomName(g->display,
							req->target));
	resp.type = SelectionNotify;
	resp.display = req->display;
	resp.requestor = req->requestor;
	resp.selection = req->selection;
	resp.target = req->target;
	resp.time = req->time;
	XSendEvent(g->display, req->requestor, 0, 0, (XEvent *) & resp);
}

void process_xevent_property(Ghandles * g, XID window, XPropertyEvent * ev)
{
	SKIP_NONMANAGED_WINDOW;
	if (g->log_level > 1)
		fprintf(stderr, "handle property %s for window 0x%x\n",
			XGetAtomName(g->display, ev->atom),
			(int) ev->window);
	if (ev->atom == XInternAtom(g->display, "WM_NAME", False))
		send_wmname(g, window);
	else if (ev->atom ==
		 XInternAtom(g->display, "WM_NORMAL_HINTS", False))
		send_wmnormalhints(g, window, 0);
	else if (ev->atom ==
		 XInternAtom(g->display, "WM_HINTS", False))
		retrieve_wmhints(g,window, 0);
	else if (ev->atom ==
		 XInternAtom(g->display, "WM_PROTOCOLS", False))
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

void process_xevent_message(Ghandles * g, XClientMessageEvent * ev)
{
	if (g->log_level > 1)
		fprintf(stderr, "handle message %s to window 0x%x\n",
			XGetAtomName(g->display, ev->message_type),
			(int) ev->window);
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
				fprintf(stderr, "ERROR process_xevent_message: Window 0x%x not initialized", (int)w);
				return;
			}
			if (g->log_level > 0)
				fprintf(stderr,
					"tray request dock for window 0x%x\n",
					(int) w);
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
				Xfree(data);

			if (!(l->data)) {
				fprintf(stderr, "ERROR process_xevent_message: Window 0x%x data not initialized", (int)w);
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
				fprintf(stderr,
					" created embeder 0x%x\n",
					(int) wd->embeder);
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
					"XReparentWindow for 0x%x failed in "
					"handle_dock, ret=0x%x\n", (int) w,
					ret);
				return;
			};

			memset(&resp, 0, sizeof(resp));
			resp.type = ClientMessage;
			resp.window = w;
			resp.message_type =
			    XInternAtom(g->display, "_XEMBED", False);
			resp.format = 32;
			resp.data.l[0] = ev->data.l[0];
			resp.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
			resp.data.l[3] = ev->window;
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
	} else if (ev->message_type == g->wm_state) {
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
			fprintf(stderr, "toggle window 0x%x property %s not supported, "
					"please report it with the application name\n", (int) ev->window,
					XGetAtomName(g->display, ev->data.l[1]));
		} else {
			fprintf(stderr, "invalid window state command (%ld) for window 0x%x"
					"report with application name\n", ev->data.l[0], (int) ev->window);
		}
		hdr.window = ev->window;
		hdr.type = MSG_WINDOW_FLAGS;
		write_message(g->vchan, hdr, msg);
	}
}

void process_xevent(Ghandles * g)
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
//      fprintf(stderr, "x=%hd y=%hd gx=%hd gy=%hd w=%hd h=%hd\n",
//        dev->area.x, dev->area.y, dev->geometry.x, dev->geometry.y, dev->area.width, dev->area.height); 
			process_xevent_damage(g, dev->drawable,
					      dev->area.x,
					      dev->area.y,
					      dev->area.width,
					      dev->area.height);
//                      fprintf(stderr, "@");
		} else if (g->log_level > 1)
			fprintf(stderr, "#");
	}

}

void send_full_window_info(Ghandles *g, XID w, struct window_data *wd)
{
	struct msg_hdr hdr;
	struct msg_create crt;
	struct msg_configure conf;
	struct msg_map_info map_info;

	XWindowAttributes attr;
	int ret;
	Window *children_list;
	Window root;
	Window parent;
	Window transient;
	unsigned int children_count;

	if (wd->is_docked)
		ret = XGetWindowAttributes(g->display, wd->embeder, &attr);
	else
		ret = XGetWindowAttributes(g->display, w, &attr);
	if (ret != 1) {
		fprintf(stderr, "XGetWindowAttributes for 0x%x failed in "
				"send_window_state, ret=0x%x\n", (int) w,
				ret);
		return;
	};
	if (wd->is_docked)
		ret = XQueryTree(g->display, wd->embeder, &root, &parent, &children_list, &children_count);
	else
		ret = XQueryTree(g->display, w, &root, &parent, &children_list, &children_count);
	if (ret != 1) {
		fprintf(stderr, "XQueryTree for 0x%x failed in "
				"send_window_state, ret=0x%x\n", (int) w,
				ret);
		return;
	};
	if (children_list)
		XFree(children_list);
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
	write_message(hdr, crt);

	hdr.type = MSG_CONFIGURE;
	conf.x = attr.x;
	conf.y = attr.y;
	conf.width = attr.width;
	conf.height = attr.height;
	conf.override_redirect = attr.override_redirect;
	write_message(hdr, conf);
	send_pixmap_mfns(g, w);

	send_wmnormalhints(g, w);

	if (wd->is_docked) {
		hdr.type = MSG_DOCK;
		hdr.untrusted_len = 0;
		write_struct(hdr);
	} else if (attr.map_state != IsUnmapped) {
		hdr.type = MSG_MAP;
		map_info.override_redirect = attr.override_redirect;
		map_info.transient_for = transient;
		write_message(hdr, map_info);
		send_wmname(g, w);
		send_window_state(g, w);
	}
}

void send_all_windows_info(Ghandles *g) {
	struct genlist *curr = windows_list->next;

	while (curr != windows_list) {
		send_full_window_info(g, curr->key, (struct window_data *)curr->data);
		curr = curr->next;
	}
}

extern void wait_for_unix_socket(int *fd);

void mkghandles(Ghandles * g)
{
	char tray_sel_atom_name[64];
	Atom net_wm_name, net_supporting_wm_check, net_supported;
	Atom supported[6];

	wait_for_unix_socket(&g->xserver_fd);	// wait for Xorg qubes_drv to connect to us
	g->display = XOpenDisplay(NULL);
	if (!g->display) {
		perror("XOpenDisplay");
		exit(1);
	}
	if (g->log_level > 0)
		fprintf(stderr,
			"Connection to local X server established.\n");
	g->screen = DefaultScreen(g->display);	/* get CRT id number */
	g->root_win = RootWindow(g->display, g->screen);	/* get default attributes */
	g->context = XCreateGC(g->display, g->root_win, 0, NULL);
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", False);
	g->wmProtocols = XInternAtom(g->display, "WM_PROTOCOLS", False);
	g->utf8_string_atom = XInternAtom(g->display, "UTF8_STRING", False);
	g->stub_win = XCreateSimpleWindow(g->display, g->root_win,
					       0, 0, 1, 1,
					       0, BlackPixel(g->display,
							     g->screen),
					       WhitePixel(g->display,
							  g->screen));
	/* pretend that GUI agent is window manager */
	net_wm_name = XInternAtom(g->display, "_NET_WM_NAME", False);
	net_supporting_wm_check = XInternAtom(g->display, "_NET_SUPPORTING_WM_CHECK", False);
	net_supported = XInternAtom(g->display, "_NET_SUPPORTED", False);
	supported[0] = net_supported;
	supported[1] = net_supporting_wm_check;
	/* _NET_WM_MOVERESIZE required to disable broken GTK+ move/resize fallback */
	supported[2] = XInternAtom(g->display, "_NET_WM_MOVERESIZE", False);
	supported[3] = XInternAtom(g->display, "_NET_WM_STATE", False);
	supported[4] = XInternAtom(g->display, "_NET_WM_STATE_FULLSCREEN", False);
	supported[5] = XInternAtom(g->display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	XChangeProperty(g->display, g->stub_win, net_wm_name, g->utf8_string_atom,
			8, PropModeReplace, (unsigned char*)"Qubes", 5);
	XChangeProperty(g->display, g->stub_win, net_supporting_wm_check, XA_WINDOW,
			32, PropModeReplace, (unsigned char*)&g->stub_win, 1);
	XChangeProperty(g->display, g->root_win, net_supporting_wm_check, XA_WINDOW,
			32, PropModeReplace, (unsigned char*)&g->stub_win, 1);
	XChangeProperty(g->display, g->root_win, net_supported, XA_ATOM,
			32, PropModeReplace, (unsigned char*)supported, sizeof(supported)/sizeof(supported[0]));

	g->clipboard_data = NULL;
	g->clipboard_data_len = 0;
	snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
		 "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display));
	g->tray_selection =
	    XInternAtom(g->display, tray_sel_atom_name, False);
	g->tray_opcode =
	    XInternAtom(g->display, "_NET_SYSTEM_TRAY_OPCODE", False);
	g->xembed_info = XInternAtom(g->display, "_XEMBED_INFO", False);
	g->wm_state = XInternAtom(g->display, "_NET_WM_STATE", False);
	g->wm_state_fullscreen = XInternAtom(g->display, "_NET_WM_STATE_FULLSCREEN", False);
	g->wm_state_demands_attention = XInternAtom(g->display, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	g->wm_take_focus = XInternAtom(g->display, "WM_TAKE_FOCUS", False);
}

void handle_keypress(Ghandles * g, XID UNUSED(winid))
{
	struct msg_keypress key;
	XkbStateRec state;
//      XKeyEvent event;
//        char buf[256];
	read_data(g->vchan, (char *) &key, sizeof(key));
#if 0
//XGetInputFocus(g->display, &focus_return, &revert_to_return);
//      fprintf(stderr, "vmside: type=%d keycode=%d currfoc=0x%x\n", key.type,
//              key.keycode, (int)focus_return);

//      XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = 1;
	event.y_root = 1;
	event.same_screen = TRUE;
	event.type = key.type;
	event.keycode = key.keycode;
	event.state = key.state;
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   KeyPressMask, (XEvent *) & event);
#else
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
#endif
//      fprintf(stderr, "win 0x%x type %d keycode %d\n",
//              (int) winid, key.type, key.keycode);
//      XSync(g->display, 0);
}

void handle_button(Ghandles * g, XID winid)
{
	struct msg_button key;
//      XButtonEvent event;
//	XWindowAttributes attr;
//	int ret;
	struct genlist *l = list_lookup(windows_list, winid);


	read_data(g->vchan, (char *) &key, sizeof(key));
	if (l && l->data && ((struct window_data*)l->data)->is_docked) {
		/* get position of embeder, not icon itself*/
		winid = ((struct window_data*)l->data)->embeder;
		XRaiseWindow(g->display, winid);
	}
#if 0
	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"do_button, ret=0x%x\n", (int) winid, ret);
		return;
	};

	XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
//      XRaiseWindow(g->display, winid);
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = attr.x + key.x;
	event.y_root = attr.y + key.y;
	event.same_screen = TRUE;
	event.type = key.type;
	event.button = key.button;
	event.state = key.state;
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   ButtonPressMask, (XEvent *) & event);
//      XSync(g->display, 0);
#endif
	if (g->log_level > 1)
		fprintf(stderr,
			"send buttonevent, win 0x%x type=%d button=%d\n",
			(int) winid, key.type, key.button);
	feed_xdriver(g, 'B', key.button, key.type == ButtonPress ? 1 : 0);
}

void handle_motion(Ghandles * g, XID winid)
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
			"XGetWindowAttributes for 0x%x failed in "
			"do_button, ret=0x%x\n", (int) winid, ret);
		return;
	};

#if 0
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = attr.x + key.x;
	event.y_root = attr.y + key.y;
	event.same_screen = TRUE;
	event.is_hint = key.is_hint;
	event.state = key.state;
	event.type = MotionNotify;
//      fprintf(stderr, "motion notify for 0x%x\n", (int)winid);
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   0, (XEvent *) & event);
//      XSync(g->display, 0);
#endif
	feed_xdriver(g, 'M', attr.x + key.x, attr.y + key.y);
}

// ensure that LeaveNotify is delivered to the window - if pointer is still
// above this window, place stub window between pointer and the window
void handle_crossing(Ghandles * g, XID winid)
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
	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"handle_crossing, ret=0x%x\n", (int) winid, ret);
		return;
	};

	if (key.type == EnterNotify) {
		// hide stub window
		XUnmapWindow(g->display, g->stub_win);
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
				"XQueryPointer for 0x%x failed in "
				"handle_crossing, ret=0x%x\n", (int) winid,
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

void take_focus(Ghandles * g, XID winid)
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
	ev.data.l[1] = CurrentTime;
	XSendEvent(ev.display, ev.window, TRUE, 0, (XEvent *) & ev);
	if (g->log_level > 0)
		fprintf(stderr, "WM_TAKE_FOCUS sent for 0x%x\n",
			(int) winid);

}

void handle_focus(Ghandles * g, XID winid)
{
	struct msg_focus key;
	struct genlist *l;
	int input_hint;
	int use_take_focus;
//      XFocusChangeEvent event;

	read_data(g->vchan, (char *) &key, sizeof(key));
#if 0
	event.display = g->display;
	event.window = winid;
	event.type = key.type;
	event.mode = key.mode;
	event.detail = key.detail;

	fprintf(stderr, "send focuschange for 0x%x type %d\n",
		(int) winid, key.type);
	XSendEvent(event.display, event.window, TRUE,
		   0, (XEvent *) & event);
#endif
	if (key.type == FocusIn
	    && (key.mode == NotifyNormal || key.mode == NotifyUngrab)) {

		XRaiseWindow(g->display, winid);

		if ( (l=list_lookup(windows_list, winid)) && (l->data) ) {
			input_hint = ((struct window_data*)l->data)->input_hint;
			use_take_focus = ((struct window_data*)l->data)->support_take_focus;
			if (((struct window_data*)l->data)->is_docked)
				XRaiseWindow(g->display, ((struct window_data*)l->data)->embeder);
		} else {
			fprintf(stderr, "WARNING handle_focus: Window 0x%x data not initialized", (int)winid);
			input_hint = True;
			use_take_focus = False;
		}

		// Give input focus only to window that set the input hint
		if (input_hint)
			XSetInputFocus(g->display, winid, RevertToParent,
			       CurrentTime);

		// Do not send take focus if the window doesn't support it
		if (use_take_focus)
			take_focus(g, winid);

		if (g->log_level > 1)
			fprintf(stderr, "0x%x raised\n", (int) winid);
	} else if (key.type == FocusOut
		   && (key.mode == NotifyNormal
		       || key.mode == NotifyUngrab)) {

		XSetInputFocus(g->display, None, RevertToParent,
			       CurrentTime);

		if (g->log_level > 1)
			fprintf(stderr, "0x%x lost focus\n", (int) winid);
	}

}

int bitset(unsigned char *keys, int num)
{
	return (keys[num / 8] >> (num % 8)) & 1;
}

void handle_keymap_notify(Ghandles * g)
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


void handle_configure(Ghandles * g, XID winid)
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

void handle_map(Ghandles * g, XID winid)
{
	struct msg_map_info inf;
	XSetWindowAttributes attr;
	read_data(g->vchan, (char *) &inf, sizeof(inf));
	attr.override_redirect = inf.override_redirect;
	XChangeWindowAttributes(g->display, winid,
				CWOverrideRedirect, &attr);
	XMapWindow(g->display, winid);
	if (g->log_level > 1)
		fprintf(stderr, "map msg for 0x%x\n", (int) winid);
}

void handle_close(Ghandles * g, XID winid)
{
	XClientMessageEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = ClientMessage;
	ev.display = g->display;
	ev.window = winid;
	ev.format = 32;
	ev.message_type = g->wmProtocols;
	ev.data.l[0] = g->wmDeleteMessage;
//        XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
	XSendEvent(ev.display, ev.window, TRUE, 0, (XEvent *) & ev);
	if (g->log_level > 0)
		fprintf(stderr, "wmDeleteMessage sent for 0x%x\n",
			(int) winid);
}

void do_execute(char *user, char *cmd)
{
	int i, fd;
	switch (fork()) {
	case -1:
		perror("fork cmd");
		break;
	case 0:
		for (i = 0; i < 256; i++)
			close(i);
		fd = open("/dev/null", O_RDWR);
		for (i = 0; i <= 2; i++)
			dup2(fd, i);
		signal(SIGCHLD, SIG_DFL);
		if (user)
			execl("/bin/su", "su", "-", user, "-c", cmd, NULL);
		else
			execl("/bin/bash", "bash", "-c", cmd, NULL);
		perror("execl cmd");
		exit(1);
	default:;
	}
}

#define CLIPBOARD_4WAY
void handle_clipboard_req(Ghandles * g, XID UNUSED(winid))
{
	Atom Clp;
	Atom QProp = XInternAtom(g->display, "QUBES_SELECTION", False);
	Atom Targets = XInternAtom(g->display, "TARGETS", False);
	Window owner;
#ifdef CLIPBOARD_4WAY
	Clp = XInternAtom(g->display, "CLIPBOARD", False);
#else
	Clp = XA_PRIMARY;
#endif
	owner = XGetSelectionOwner(g->display, Clp);
	if (g->log_level > 0)
		fprintf(stderr, "clipboard req, owner=0x%x\n",
			(int) owner);
	if (owner == None) {
		send_clipboard_data(g->vchan, NULL, 0);
		return;
	}
	XConvertSelection(g->display, Clp, Targets, QProp,
			  g->stub_win, CurrentTime);
}

void handle_clipboard_data(Ghandles * g, int len)
{
	Atom Clp = XInternAtom(g->display, "CLIPBOARD", False);

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
	XSetSelectionOwner(g->display, XA_PRIMARY, g->stub_win,
			   CurrentTime);
	XSetSelectionOwner(g->display, Clp, g->stub_win, CurrentTime);
#ifndef CLIPBOARD_4WAY
	XSync(g->display, False);
	feed_xdriver(g, 'B', 2, 1);
	feed_xdriver(g, 'B', 2, 0);
#endif
}

void handle_window_flags(Ghandles *g, XID winid)
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
	ret = XGetWindowProperty(g->display, winid, g->wm_state, 0, 10,
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

	XChangeProperty(g->display, winid, g->wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)new_state_list, j);
}

void handle_message(Ghandles * g)
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
		handle_clipboard_data(g, hdr.window);
		break;
	case MSG_KEYMAP_NOTIFY:
		handle_keymap_notify(g);
		break;
	case MSG_WINDOW_FLAGS:
		handle_window_flags(g, hdr.window);
		break;
	default:
		fprintf(stderr, "got unknown msg type %d, ignoring\n", hdr.type);
		while (hdr.untrusted_len > 0) {
			hdr.untrusted_len -= read_data(g->vchan, discard, min(hdr.untrusted_len, sizeof(discard)));
		}
	}
}

void get_xconf_and_run_x(libvchan_t *vchan)
{
	struct msg_xconf xconf;
	char val[64];
	read_struct(vchan, xconf);
	snprintf(val, sizeof(val), "%d", xconf.w);
	setenv("W", val, 1);
	snprintf(val, sizeof(val), "%d", xconf.h);
	setenv("H", val, 1);
	snprintf(val, sizeof(val), "%d", xconf.mem);
	setenv("MEM", val, 1);
	snprintf(val, sizeof(val), "%d", xconf.depth);
	setenv("DEPTH", val, 1);
	do_execute(NULL, "/usr/bin/qubes-run-xorg.sh");
}

void send_protocol_version(libvchan_t *vchan)
{
	uint32_t version = QUBES_GUI_PROTOCOL_VERSION_LINUX;
	write_struct(vchan, version);
}

void handle_guid_disconnect()
{
	Ghandles *g = ghandles_for_vchan_reinitialize;
	struct msg_xconf xconf;

	vchan_cleanup();
	wait_for_possible_dispvm_resume();
	peer_server_reinitialize(6000);
	send_protocol_version();
	/* discard */
	read_struct(xconf);
	send_all_windows_info(g);
}

void usage()
{
	fprintf(stderr, "Usage: qubes_gui [-v] [-q] [-h]\n");
	fprintf(stderr, "       -v  increase log verbosity\n");
	fprintf(stderr, "       -q  decrease log verbosity\n");
	fprintf(stderr, "       -m  sync all modifiers before key event (default: only Caps Lock)\n");
	fprintf(stderr, "       -h  print this message\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Log levels:\n");
	fprintf(stderr, " 0 - only errors\n");
	fprintf(stderr, " 1 - some basic messages (default)\n");
	fprintf(stderr, " 2 - debug\n");
}

void parse_args(Ghandles * g, int argc, char **argv)
{
	int opt;

	// defaults
	g->log_level = 0;
	g->sync_all_modifiers = 0;
	while ((opt = getopt(argc, argv, "qvhm")) != -1) {
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
		case 'h':
			usage();
			exit(0);
		default:
			usage();
			exit(1);
		}
	}
}

int main(int argc, char **argv)
{
	int i;
	int xfd;
	Ghandles g;

	/* FIXME: 0 is remote domain */
	g.vchan = libvchan_server_init(0, 6000, 4096, 4096);
	if (!g.vchan) {
		fprintf(stderr, "vchan initialization failed\n");
		exit(1);
	}
	/* wait for gui daemon */
	while (!libvchan_is_open(g.vchan))
		libvchan_wait(g.vchan);
	saved_argv = argv;
	vchan_register_at_eof(handle_guid_disconnect);
	send_protocol_version(g.vchan);
	get_xconf_and_run_x(g.vchan);
	mkghandles(&g);
	ghandles_for_vchan_reinitialize = &g;
	parse_args(&g, argc, argv);
	for (i = 0; i < ScreenCount(g.display); i++)
		XCompositeRedirectSubwindows(g.display,
					     RootWindow(g.display, i),
//                                           CompositeRedirectAutomatic);
					     CompositeRedirectManual);
	for (i = 0; i < ScreenCount(g.display); i++)
		XSelectInput(g.display, RootWindow(g.display, i),
			     SubstructureNotifyMask);


	if (!XDamageQueryExtension(g.display, &damage_event,
				   &damage_error)) {
		perror("XDamageQueryExtension");
		exit(1);
	}
	XAutoRepeatOff(g.display);
	signal(SIGCHLD, SIG_IGN);
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
		ev.message_type = XInternAtom(g.display, "MANAGER", False);
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
	for (;;) {
		int busy;
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
		wait_for_vchan_or_argfd(g.vchan, 1, &xfd, NULL);
	}
	return 0;
}

// vim:noet:
