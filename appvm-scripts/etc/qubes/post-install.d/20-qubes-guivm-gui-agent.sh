#!/usr/bin/sh

if [ -e /usr/bin/Xephyr ]; then
    qvm-features-request supported-service.guivm-gui-agent=1
fi
