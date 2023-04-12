#!/bin/bash

set -eo pipefail

# when changing, bump the CSS_VERSION below to force updating user config
CSS='/* QUBES BEGIN */
/* Do not modify text until end marker, it will get overriden on update */
/* See https://github.com/QubesOS/qubes-issues/issues/8081#issuecomment-1473412028 */
.solid-csd popover > contents, .solid-csd .hover-assistant, .solid-csd .completion {
	box-shadow: none;
	border-radius: 0px;
	margin: 0px;
}
popover > contents {
    border-style: none;
}

popover > arrow {
	border-style: none;
}
popover {
	background-color: @view_bg_color;
}

/* performance improvement with software rendering */
window {
	box-shadow: none;
	padding: 0px;
}

/* 
 * Spinner monopolize the whole idle loop, see
 * https://github.com/QubesOS/qubes-issues/issues/7921
 */
spinner {
    animation-play-state: paused;
    opacity: 0;
}
/* QUBES END */'

CSS_PATH="$HOME/.config/gtk-4.0/gtk.css"
CSS_VERSION=1
CSS_FLAG_FILE="$HOME/.config/gtk-4.0/qubes-patched"

if [ -r "$CSS_FLAG_FILE" ] && [ "$(<"$CSS_FLAG_FILE")" -eq "$CSS_VERSION" ]; then
    # already patched
    exit 0
fi


if [ ! -e "$CSS_PATH" ] || ! grep -q "QUBES BEGIN" "$CSS_PATH"; then
    mkdir -p "${CSS_PATH%/*}"
    echo "$CSS" >> "$CSS_PATH"
else
    echo "$CSS" | sed -i -e "/QUBES BEGIN/,/QUBES END/{r /dev/stdin" -e "; d }" "$CSS_PATH"
fi
echo "$CSS_VERSION" > "$CSS_FLAG_FILE"
