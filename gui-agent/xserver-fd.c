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

#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#define SOCKET_ADDRESS  "/var/run/xf86-qubes-socket"
void wait_for_unix_socket(int *fd)
{
    struct sockaddr_un sockname, peer;
    int s;
    unsigned int addrlen;
    int prev_umask;

    unlink(SOCKET_ADDRESS);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sockname, 0, sizeof(sockname));
    sockname.sun_family = AF_UNIX;
    memcpy(sockname.sun_path, SOCKET_ADDRESS, strlen(SOCKET_ADDRESS));

    prev_umask=umask(077);
    if (bind(s, (struct sockaddr *) &sockname, sizeof(sockname)) == -1) {
        printf("bind() failed\n");
        close(s);
        exit(1);
    }
    umask(prev_umask);
    //	chmod(sockname.sun_path, 0666);
    if (listen(s, 5) == -1) {
        perror("listen() failed\n");
        close(s);
        exit(1);
    }

    addrlen = sizeof(peer);
    fprintf (stderr, "Waiting on %s socket...\n", SOCKET_ADDRESS);
    *fd = accept(s, (struct sockaddr *) &peer, &addrlen);
    if (*fd == 1) {
        perror("unix accept");
        exit(1);
    }
    fprintf (stderr, "Ok, somebody connected.\n");
    close(s);
    //	sleep(2);		//give xserver time to boot up
}
