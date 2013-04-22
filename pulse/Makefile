CC=gcc
CFLAGS=-Wall -Wextra -Werror -O2
VCHANLIBS=`pkg-config --libs vchan-$(BACKEND_VMM)`
VCHANCFLAGS=`pkg-config --cflags vchan-$(BACKEND_VMM)`
GLIBCFLAGS=`pkg-config --cflags glib-2.0` `pkg-config --cflags dbus-glib-1`
GLIBLIBS=`pkg-config --libs glib-2.0` `pkg-config --libs dbus-glib-1` `pkg-config --libs gthread-2.0`
all: module-vchan-sink.so
module-vchan-sink.so: module-vchan-sink.o
	$(CC) -shared -o module-vchan-sink.so module-vchan-sink.o \
		$(VCHANLIBS)
module-vchan-sink.o: module-vchan-sink.c
	$(CC) $(CFLAGS) -g -c $(VCHANCFLAGS) -fPIC module-vchan-sink.c  -DHAVE_CONFIG_H -I.
clean:
	rm -f *.so *.o *~
