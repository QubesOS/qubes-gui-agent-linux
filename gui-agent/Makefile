#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#

CC ?= gcc
CFLAGS += -I../include/ `pkg-config --cflags vchan` -g -Wall -Wextra -Werror -fPIC \
	  -Wmissing-prototypes -Wstrict-prototypes -Wold-style-declaration \
	  -Wold-style-definition
OBJS = vmside.o txrx-vchan.o error.o list.o encoding.o
LIBS = -lX11 -lXdamage -lXcomposite -lXfixes `pkg-config --libs vchan` -lqubesdb \
	   -lunistring


all: qubes-gui qubes-gui-runuser
qubes-gui: $(OBJS)
	$(CC) $(LDFLAGS) -pie -g -o qubes-gui $(OBJS) \
		$(LIBS)
qubes-gui-runuser: CFLAGS += -g -Wall -Wextra -Werror -pie -fPIC
qubes-gui-runuser: LDLIBS += -lpam -lqubesdb
qubes-gui-runuser: qubes-gui-runuser.c
clean:
	rm -f qubes-gui qubes-gui-runuser ./*.o ./*~
