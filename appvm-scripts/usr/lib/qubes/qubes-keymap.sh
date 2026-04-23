#!/bin/sh

# This file may be also executed by qubes-change-keyboard-layout

# User-saved layout file (written by qubes-change-keyboard-layout)
CUSTOM_LAYOUT_FILE="${HOME:-/root}/.config/qubes-keyboard-layout.rc"

set_keyboard_layout() {
    KEYMAP="$1"
    if [ -z "$KEYMAP" ]; then
        KEYMAP=us
    fi
    KEYMAP_LAYOUT="$(echo "$KEYMAP"+ | cut -f 1 -d +)"
    KEYMAP_VARIANT="$(echo "$KEYMAP"+ | cut -f 2 -d +)"
    KEYMAP_OPTIONS="$(echo "$KEYMAP"+ | cut -f 3 -d +)"
    if [ "$KEYMAP_LAYOUT" != "us" -a "$KEYMAP_LAYOUT" != "si" ]; then
        KEYMAP_LAYOUT="$KEYMAP_LAYOUT,us"
        KEYMAP_VARIANT="$KEYMAP_VARIANT,"
    fi
    if [ -n "$KEYMAP_VARIANT" ]; then
        KEYMAP_VARIANT="-variant $KEYMAP_VARIANT"
    fi
    if [ -n "$KEYMAP_OPTIONS" ]; then
        KEYMAP_OPTIONS="-option -option $KEYMAP_OPTIONS"
    fi
    for x in /tmp/.X11-unix/X*; do
        display="$(basename "$x")"
        setxkbmap -display ":${display#X}" -layout "$KEYMAP_LAYOUT" \
            $KEYMAP_VARIANT $KEYMAP_OPTIONS
    done
}

# Read current live X11 keymap, return as layout+variant+options string
get_live_layout() {
    for x in /tmp/.X11-unix/X*; do
        display="$(basename "$x")"
        QUERY="$(setxkbmap -display ":${display#X}" -query 2>/dev/null)" || continue
        LAYOUT="$(echo "$QUERY"  | awk '/^layout:/  {sub(/^layout:[[:space:]]+/,""); print}')"
        VARIANT="$(echo "$QUERY" | awk '/^variant:/ {sub(/^variant:[[:space:]]+/,""); print}')"
        OPTIONS="$(echo "$QUERY" | awk '/^options:/ {sub(/^options:[[:space:]]+/,""); print}')"
        if [ -n "$LAYOUT" ]; then
            echo "${LAYOUT}+${VARIANT}+${OPTIONS}"
            return
        fi
    done
}

get_effective_layout() {
    # Priority 1: user explicitly saved a layout via qubes-change-keyboard-layout
    if [ -r "$CUSTOM_LAYOUT_FILE" ]; then
        cat "$CUSTOM_LAYOUT_FILE"
        return
    fi
    # Priority 2: preserve whatever X11 currently has (survives NetVM changes)
    LIVE="$(get_live_layout)"
    if [ -n "$LIVE" ]; then
        echo "$LIVE"
        return
    fi
    # Priority 3: fall back to dom0/GuiVM default
    /usr/bin/qubesdb-read /keyboard-layout 2>/dev/null
}

# On first start: apply dom0 default unconditionally (normal startup behaviour)
QUBES_KEYMAP="$(/usr/bin/qubesdb-read /keyboard-layout 2>/dev/null)"
if [ -n "$QUBES_KEYMAP" ]; then
    set_keyboard_layout "$QUBES_KEYMAP"
fi

# On subsequent QubesDB changes (e.g. NetVM change): preserve user's layout
while qubesdb-watch /keyboard-layout; do
    QUBES_KEYMAP="$(get_effective_layout)"
    if [ -n "$QUBES_KEYMAP" ]; then
        set_keyboard_layout "$QUBES_KEYMAP"
    fi
done
