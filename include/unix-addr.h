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

#include <sys/un.h>

static inline socklen_t sockaddr_un_from_path(struct sockaddr_un *addr, char *path)
{
    size_t len;

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    len = strlen(path);
    if (len == 0 || len > sizeof(addr->sun_path) - 1) {
        return 0;
    }
    memcpy(addr->sun_path, path, len);
    return offsetof(typeof(*addr), sun_path) + len + 1;
}
