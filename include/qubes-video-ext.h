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

#include <X11/Xproto.h>
#include <assert.h>

// This X extension is only for internal use. It's used to send custom events
// from the video driver to the agent. Since video driver and agent are updated
// in lockstep we don't need any version handling.

#define QVE_NAME "_qubes-video-ext"

#define X_QVERegister 0
#define X_QVEUnregister 1
#define XQVENumberRequests 2

#define QVEWindowRealized 0
#define QVENumberEvents 1

typedef struct {
    CARD8 reqType;
    CARD8 QVEReqType;
    CARD16 length;
} xQVEReq;

#define sz_xQVEReq sizeof(xQVEReq)

static_assert(sizeof(xQVEReq) >= sizeof(xReq));

#define QVEWindowRealizedDetailUnrealized 1

typedef struct {
    BYTE type;
    BYTE detail;
    CARD16 sequenceNumber;
    CARD32 window;
    CARD32 pad1;
    CARD32 pad2;
    CARD32 pad3;
    CARD32 pad4;
    CARD32 pad5;
    CARD32 pad6;
} xQVEWindowRealizedEvent;

static_assert(sizeof(xQVEWindowRealizedEvent) == sizeof(xEvent));
