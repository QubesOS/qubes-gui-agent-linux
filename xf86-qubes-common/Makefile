XORG_SERVER_CFLAGS=$(shell pkg-config --cflags xorg-server)

all: libxf86-qubes-common.so

libxf86-qubes-common.so: xf86-qubes-common.c include/xf86-qubes-common.h
	gcc -Wall -shared -fPIC -DPIC $(XORG_SERVER_CFLAGS) $(CFLAGS) $(LDFLAGS) \
		-o libxf86-qubes-common.so xf86-qubes-common.c

clean:
	rm -f *.so
