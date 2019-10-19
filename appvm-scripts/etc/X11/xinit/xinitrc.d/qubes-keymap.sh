#!/bin/sh

# This file may be also executed by qubes-change-keyboard-layout

QUBES_KEYMAP="$(/usr/bin/qubesdb-read /keyboard-layout)"
QUBES_USER_KEYMAP="$(cat "$HOME/.config/qubes-keyboard-layout.rc" 2> /dev/null)"

if [ -n "$QUBES_KEYMAP" ]; then
    setxkbmap -layout "$QUBES_KEYMAP"
fi

if [ -n "$QUBES_USER_KEYMAP" ]; then
    QUBES_USER_KEYMAP_LAYOUT="$(echo "$QUBES_USER_KEYMAP"+ | cut -f 1 -d +)"
    QUBES_USER_KEYMAP_VARIANT="$(echo "$QUBES_USER_KEYMAP"+ | cut -f 2 -d +)"
    if [ -n "$QUBES_USER_KEYMAP_VARIANT" ]; then
        QUBES_USER_KEYMAP_VARIANT="-variant $QUBES_USER_KEYMAP_VARIANT"
    fi
    setxkbmap "$QUBES_USER_KEYMAP_LAYOUT" "$QUBES_USER_KEYMAP_VARIANT"
fi
