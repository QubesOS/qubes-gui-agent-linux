#!/bin/sh

QUBES_KEYMAP="`/usr/bin/xenstore-read qubes-keyboard`"
QUBES_KEYMAP="`echo -e $QUBES_KEYMAP`"
QUBES_USER_KEYMAP=`cat $HOME/.config/qubes-keyboard-layout.rc 2> /dev/null`

if [ -n "$QUBES_KEYMAP" ]; then
    echo "$QUBES_KEYMAP" | xkbcomp - $DISPLAY
fi

if [ -n "$QUBES_USER_KEYMAP" ]; then
    setxkbmap $QUBES_USER_KEYMAP
fi
