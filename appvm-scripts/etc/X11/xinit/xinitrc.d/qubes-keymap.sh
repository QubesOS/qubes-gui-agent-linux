#!/bin/sh

if [ -x /usr/sbin/xenstore-read ]; then
        XENSTORE_READ="/usr/sbin/xenstore-read"
else
        XENSTORE_READ="/usr/bin/xenstore-read"
fi

QUBES_KEYMAP="`$XENSTORE_READ qubes-keyboard`"
QUBES_KEYMAP="`/bin/echo -e $QUBES_KEYMAP`"
QUBES_USER_KEYMAP=`cat $HOME/.config/qubes-keyboard-layout.rc 2> /dev/null`

if [ -n "$QUBES_KEYMAP" ]; then
    echo "$QUBES_KEYMAP" | xkbcomp - $DISPLAY
fi

if [ -n "$QUBES_USER_KEYMAP" ]; then
    QUBES_USER_KEYMAP_LAYOUT=`echo $QUBES_USER_KEYMAP+ | cut -f 1 -d +`
    QUBES_USER_KEYMAP_VARIANT=`echo $QUBES_USER_KEYMAP+ | cut -f 2 -d +`
    if [ -n "$QUBES_USER_KEYMAP_VARIANT" ]; then
        QUBES_USER_KEYMAP_VARIANT="-variant $QUBES_USER_KEYMAP_VARIANT"
    fi
    setxkbmap $QUBES_USER_KEYMAP_LAYOUT $QUBES_USER_KEYMAP_VARIANT
fi
