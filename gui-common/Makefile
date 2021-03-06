ROOT_DIR := $(patsubst %/gui-common/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

CC ?= gcc
CFLAGS += -g -Wall -Wextra -Werror -pie -fPIC -ffile-prefix-map=$(ROOT_DIR)=.
LDLIBS = -lpam

all: qubes-gui-runuser

qubes-gui-runuser: qubes-gui-runuser.c

clean:
	rm -f *.o qubes-gui-runuser
