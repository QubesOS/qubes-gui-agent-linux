#!/bin/sh

. /usr/lib/qubes/init/functions

user=$(qubesdb-read /default-user) || exit
# pretend tha user is at local console
mkdir -p /var/run/console
: > "/var/run/console/$user"

# set corresponding display for guivm
if qsvc guivm-gui-agent; then
    sed -i '/DISPLAY=/d' /var/run/qubes-service-environment
    echo DISPLAY=:1 >> /var/run/qubes-service-environment
fi

# set gui opts
gui_xid="$(qubesdb-read -w /qubes-gui-domain-xid)"
if [ -z "$gui_xid" ]; then
    gui_xid=0
fi
gui_opts="-d $gui_xid"

debug_mode=$(qubesdb-read /qubes-debug-mode 2> /dev/null)
if [ -n "$debug_mode" ] && [ "$debug_mode" -gt 0 ]; then
    gui_opts="$gui_opts -vv"
fi

echo "GUI_OPTS=$gui_opts" >> /var/run/qubes-service-environment

# 2**30
echo 1073741824 > /sys/module/xen_gntalloc/parameters/limit
