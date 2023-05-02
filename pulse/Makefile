CC=gcc
CFLAGS+=-Wall -Wextra -Werror -O2
LIBS=-lqubesdb
VCHANLIBS=`pkg-config --libs vchan`
VCHANCFLAGS=`pkg-config --cflags vchan`
GLIBCFLAGS=`pkg-config --cflags glib-2.0` `pkg-config --cflags dbus-glib-1`
GLIBLIBS=`pkg-config --libs glib-2.0` `pkg-config --libs dbus-glib-1` `pkg-config --libs gthread-2.0`
LIBPULSE_VERSION=$(shell pkg-config --modversion libpulse)
all: module-vchan-sink.so
module-vchan-sink.so: module-vchan-sink.o
	$(CC) -shared -o module-vchan-sink.so module-vchan-sink.o \
		$(VCHANLIBS) $(LIBS)
module-vchan-sink.o: module-vchan-sink.c
	$(CC) $(CFLAGS) -g -c $(VCHANCFLAGS) -fPIC module-vchan-sink.c  -DHAVE_CONFIG_H -DHAVE_QUBESDB_CLIENT_H -DLIBPULSE_VERSION='"$(LIBPULSE_VERSION)"' -I.
clean:
	rm -f *.so *.o *~
