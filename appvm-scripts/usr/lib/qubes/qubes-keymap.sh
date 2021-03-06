#!/usr/bin/sh

# This file may be also executed by qubes-change-keyboard-layout

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
        KEYMAP_OPTIONS="-option $KEYMAP_OPTIONS"
    fi

    # Set layout on all DISPLAY
    for x in /tmp/.X11-unix/X*
    do
        display="$(basename "$x")"
        setxkbmap -display ":${display#X}" -layout "$KEYMAP_LAYOUT" $KEYMAP_VARIANT $KEYMAP_OPTIONS
    done
}

QUBES_KEYMAP="$(/usr/bin/qubesdb-read /keyboard-layout)"

if [ -n "$QUBES_KEYMAP" ]; then
  set_keyboard_layout "$QUBES_KEYMAP"
fi

while qubesdb-watch /keyboard-layout ; do
  QUBES_KEYMAP="$(/usr/bin/qubesdb-read /keyboard-layout)"
  if [ -n "$QUBES_KEYMAP" ]; then
    set_keyboard_layout "$QUBES_KEYMAP"
  fi
done
