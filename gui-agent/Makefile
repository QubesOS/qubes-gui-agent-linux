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

ROOT_DIR := $(patsubst %/gui-agent/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
GUI_COMMON_DIR = $(realpath $(ROOT_DIR)/gui-common)

CC ?= gcc
CFLAGS += -I$(ROOT_DIR)/include/ `pkg-config --cflags vchan` -g -Wall -Wextra -Werror -pie -fPIC -ffile-prefix-map=$(ROOT_DIR)=.
OBJS = vmside.o $(GUI_COMMON_DIR)/txrx-vchan.o $(GUI_COMMON_DIR)/error.o $(ROOT_DIR)/common/list.o $(GUI_COMMON_DIR)/encoding.o
LIBS = -lX11 -lXdamage -lXcomposite -lXfixes `pkg-config --libs vchan` -lqubesdb

all: qubes-gui
qubes-gui: $(OBJS)
	$(CC) $(LDFLAGS) -pie -g -o qubes-gui $(OBJS) $(LIBS)
clean:
	rm -f qubes-gui *.o *~ $(ROOT_DIR)/common/list.o
