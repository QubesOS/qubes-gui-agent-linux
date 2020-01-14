#!/bin/sh

. /usr/lib/qubes/init/functions

# pretend tha user is at local console
mkdir -p /var/run/console ; /bin/touch /var/run/console/user

# set corresponding display for guivm
if qsvc guivm-gui-agent; then
    sed -i '/DISPLAY=/d' /var/run/qubes-service-environment
    echo DISPLAY=:1 >> /var/run/qubes-service-environment
fi

while [ -z "$(qubesdb-read /qubes-gui-domain-xid > /dev/null 2>&1)" ];
do
    sleep 1
done

# set gui opts
gui_opts="-d $(qubesdb-read /qubes-gui-domain-xid)"

debug_mode=$(qubesdb-read /qubes-debug-mode 2> /dev/null)
if [ -n "$debug_mode" ] && [ "$debug_mode" -gt 0 ]; then
    gui_opts="$gui_opts -vv"
fi

echo "GUI_OPTS=$gui_opts" >> /var/run/qubes-service-environment