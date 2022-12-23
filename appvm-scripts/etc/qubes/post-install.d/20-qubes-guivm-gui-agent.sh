#!/bin/sh

if [ -e /usr/bin/Xephyr ]; then
    qvm-features-request supported-service.guivm-gui-agent=1
fi

qvm-features-request supported-service.gui-agent-clipboard-wipe=1
