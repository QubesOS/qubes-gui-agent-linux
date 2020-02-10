#!/bin/sh

# This file may be also executed by qubes-change-keyboard-layout

QUBES_KEYMAP_LEGACY="$(/usr/bin/qubesdb-read /qubes-keyboard)"
QUBES_KEYMAP="$(/usr/bin/qubesdb-read /keyboard-layout)"
QUBES_USER_KEYMAP="$(cat "$HOME/.config/qubes-keyboard-layout.rc" 2> /dev/null)"

set_keyboard_layout() {
    KEYMAP="$1"
    # Default value
    if [ -z "$KEYMAP" ]; then
        KEYMAP=us
    fi
    KEYMAP_LAYOUT="$(echo "$KEYMAP"+ | cut -f 1 -d +)"
    KEYMAP_VARIANT="$(echo "$KEYMAP"+ | cut -f 2 -d +)"
    KEYMAP_OPTIONS="$(echo "$KEYMAP"+ | cut -f 3 -d +)"
    if [ -n "$KEYMAP_VARIANT" ]; then
        KEYMAP_VARIANT="-variant $KEYMAP_VARIANT"
    fi

    if [ -n "$KEYMAP_OPTIONS" ]; then
        KEYMAP_OPTIONS="-options $KEYMAP_OPTIONS"
    fi

    # Set layout on all DISPLAY
    for x in /tmp/.X11-unix/X*
    do
        display="$(basename "$x")"
        setxkbmap -display ":${display#X}" -layout "$KEYMAP_LAYOUT" $KEYMAP_VARIANT $KEYMAP_OPTIONS
    done
}

if [ -n "$QUBES_KEYMAP_LEGACY" ]; then
	echo -e "$QUBES_KEYMAP_LEGACY" | xkbcomp - $DISPLAY
fi

if [ -n "$QUBES_KEYMAP" ]; then
    set_keyboard_layout "$QUBES_KEYMAP"
fi

if [ -n "$QUBES_USER_KEYMAP" ]; then
    set_keyboard_layout "$QUBES_USER_KEYMAP"
fi
