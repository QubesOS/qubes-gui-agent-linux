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
int write_data(char *buf, int size);
int real_write_message(char *hdr, int size, char *data, int datasize);
int read_data(char *buf, int size);
int read_ready();
#define read_struct(x) read_data((char*)&x, sizeof(x))
#define write_struct(x) write_data((char*)&x, sizeof(x))
#define write_message(x,y) do {\
	x.untrusted_len = sizeof(y); \
	real_write_message((char*)&x, sizeof(x), (char*)&y, sizeof(y)); \
    } while(0)
void wait_for_vchan_or_argfd(int nfd, int *fd, fd_set * retset);
int peer_server_init(int port);
char *get_vm_name(int dom, int *target_domid);
void peer_client_init(int dom, int port);
int peer_server_reinitialize(int port);
void vchan_register_at_eof(void (*new_vchan_at_eof)(void));
void vchan_close();
int vchan_fd();
void wait_for_possible_dispvm_resume(void);
void vchan_cleanup(void);

#endif /* _QUBES_TXRX_H */
