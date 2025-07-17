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

#pragma once

#include "qubes-video-ext.h"
#include <X11/Xlib.h>

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Bool unrealized;
} XQVEWindowRealizedEvent;

Bool XQVERegister(Display *dpy);
Bool XQVEQueryExtension(Display *dpy, int *event_base);
