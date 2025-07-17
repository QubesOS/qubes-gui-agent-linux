/*
 * The Qubes OS Project, https://www.qubes-os.org/
 *
 * Copyright (C) 2025  Simon Gaiser <simon@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "video-ext-client.h"

#include <X11/Xlibint.h>
#include <X11/extensions/Xext.h>
#include <X11/extensions/extutil.h>

static int close_display(Display *dpy, XExtCodes *codes);
static Bool wire_to_event(Display *dpy, XEvent *libEv, xEvent *wireEv);

static XExtensionHooks ext_hooks = {
    NULL,          // create_gc
    NULL,          // copy_gc
    NULL,          // flush_gc
    NULL,          // free_gc
    NULL,          // create_font
    NULL,          // free_font
    close_display, // close_display
    wire_to_event, // wire_to_event
    NULL,          // event_to_wire (we don't need SendEvent)
    NULL,          // error
    NULL,          // error_string
};

static XExtensionInfo _ext_info;
static XExtensionInfo *ext_info = &_ext_info;
static const char * ext_name = QVE_NAME;

#define QVECheckExtension(dpy, info, val) \
    XextCheckExtension(dpy, info, ext_name, val)

static XEXT_GENERATE_FIND_DISPLAY(find_display,
                                  ext_info,
                                  ext_name,
                                  &ext_hooks,
                                  QVENumberEvents,
                                  NULL);

static XEXT_GENERATE_CLOSE_DISPLAY(close_display, ext_info);

static Bool
wire_to_event(Display *dpy, XEvent *libEv, xEvent *wireEv)
{
    XExtDisplayInfo *info = find_display(dpy);

    QVECheckExtension(dpy, info, False);

    BYTE type = wireEv->u.u.type;
    if ((type & 0x7f) != info->codes->first_event + QVEWindowRealized) {
        return False;
    }

    XQVEWindowRealizedEvent *qLibEv = (XQVEWindowRealizedEvent *)libEv;
    xQVEWindowRealizedEvent *qWireEv = (xQVEWindowRealizedEvent *)wireEv;

    qLibEv->type = type & 0x7f;
    qLibEv->serial = _XSetLastRequestRead(dpy, (xGenericReply *)wireEv);
    qLibEv->send_event = (type & 0x80) != 0;
    qLibEv->display = dpy;
    qLibEv->window = qWireEv->window;
    qLibEv->unrealized =
        (qWireEv->detail & QVEWindowRealizedDetailUnrealized) != 0;

    return True;
}

Bool
XQVEQueryExtension(Display *dpy, int *event_base)
{
    XExtDisplayInfo *info = find_display(dpy);

    if (!XextHasExtension(info)) {
        return False;
    }

    *event_base = info->codes->first_event;
    return True;
}

Bool
XQVERegister(Display *dpy)
{
    XExtDisplayInfo *info = find_display(dpy);

    QVECheckExtension(dpy, info, False);

    LockDisplay(dpy);
    xQVEReq *req;
#define X_QVE X_QVERegister
    GetReq(QVE, req);
#undef X_QVE
    req->reqType = info->codes->major_opcode;
    req->QVEReqType = X_QVERegister;
    UnlockDisplay(dpy);
    SyncHandle();

    return True;
}
