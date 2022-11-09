/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <libvchan.h>
#include <errno.h>
#include <poll.h>
#include <err.h>

#include "txrx.h"

static void (*vchan_at_eof)(void) = NULL;

void vchan_register_at_eof(void (*new_vchan_at_eof)(void))
{
    vchan_at_eof = new_vchan_at_eof;
}

static _Noreturn void handle_vchan_error(libvchan_t *vchan, const char *op)
{
    if (!libvchan_is_open(vchan) && vchan_at_eof)
        vchan_at_eof();
    errx(1, "Error while vchan %s\n, terminating", op);
}

int real_write_message(libvchan_t *vchan, char *hdr, int size, char *data, int datasize)
{
    if (libvchan_send(vchan, hdr, size) < 0)
        handle_vchan_error(vchan, "send hdr");
    if (libvchan_send(vchan, data, datasize) < 0)
        handle_vchan_error(vchan, "send data");
    return 0;
}

int write_data(libvchan_t *vchan, char *buf, int size)
{
    int written = 0;
    int ret;

    while (written < size) {
        /* cannot use libvchan_send b/c buf can be bigger than ring buffer */
        ret = libvchan_write(vchan, buf + written, size - written);
        if (ret <= 0)
            handle_vchan_error(vchan, "write data");
        written += ret;
    }
    //      fprintf(stderr, "sent %d bytes\n", size);
    return size;
}

int read_data(libvchan_t *vchan, char *buf, int size)
{
    int written = 0;
    int ret;
    while (written < size) {
        ret = libvchan_read(vchan, buf + written, size - written);
        if (ret <= 0)
            handle_vchan_error(vchan, "read data");
        written += ret;
    }
    //      fprintf(stderr, "read %d bytes\n", size);
    return size;
}

static int wait_for_vchan_or_argfd_once(libvchan_t *vchan, struct pollfd *const fds, size_t const nfds)
{
    int ret;
    ret = poll(fds, nfds, 1000);
    if (ret < 0) {
        if (errno == EINTR)
            return -1;
        err(1, "poll");
    }
    if (!libvchan_is_open(vchan)) {
        fprintf(stderr, "libvchan_is_eof\n");
        if (vchan_at_eof != NULL) {
            vchan_at_eof();
            return -1;
        } else
            exit(0);
    }
    if (fds[0].revents) {
        // the following will never block; we need to do this to
        // clear libvchan_fd pending state 
        libvchan_wait(vchan);
    }
    return ret;
}

int wait_for_vchan_or_argfd(libvchan_t *vchan, struct pollfd *const fds, size_t nfds)
{
    int ret;
    while ((ret=wait_for_vchan_or_argfd_once(vchan, fds, nfds)) == 0);
    return ret;
}
