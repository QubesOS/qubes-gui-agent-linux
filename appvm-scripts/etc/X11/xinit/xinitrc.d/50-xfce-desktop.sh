#!/bin/sh

# shellcheck disable=SC1091
. /usr/lib/qubes/init/functions

if ! qsvc guivm-gui-agent; then

XDG_MENU_PREFIX="xfce-"
export XDG_MENU_PREFIX

DESKTOP_SESSION="xfce"
export DESKTOP_SESSION

XDG_CURRENT_DESKTOP="XFCE"
export XDG_CURRENT_DESKTOP

XDG_CONFIG_HOME=$HOME/.config
[ -d "$XDG_CONFIG_HOME" ] || mkdir "$XDG_CONFIG_HOME"

XDG_CACHE_HOME=$HOME/.cache
[ -d "$XDG_CACHE_HOME" ] || mkdir "$XDG_CACHE_HOME"

if which xdg-user-dirs-update >/dev/null 2>&1; then
    xdg-user-dirs-update
fi

export QT_QPA_PLATFORMTHEME=qt5ct

xfsettingsd &
fi