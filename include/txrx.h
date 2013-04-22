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

#ifndef _QUBES_TXRX_H
#define _QUBES_TXRX_H

#include <sys/select.h>
#include <libvchan.h>

int write_data(libvchan_t *vchan, char *buf, int size);
int real_write_message(libvchan_t *vchan, char *hdr, int size, char *data, int datasize);
int read_data(libvchan_t *vchan, char *buf, int size);
#define read_struct(vchan, x) read_data(vchan, (char*)&x, sizeof(x))
#define write_struct(vchan, x) write_data(vchan, (char*)&x, sizeof(x))
#define write_message(vchan,x,y) do {\
	x.untrusted_len = sizeof(y); \
	real_write_message(vchan, (char*)&x, sizeof(x), (char*)&y, sizeof(y)); \
    } while(0)
void wait_for_vchan_or_argfd(libvchan_t *vchan, int nfd, int *fd, fd_set * retset);
void vchan_register_at_eof(void (*new_vchan_at_eof)(void));

#endif /* _QUBES_TXRX_H */
