#!/bin/sh
#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#

#expects W, H, MEM and DEPTH env vars to be set by invoker
DUMMY_MAX_CLOCK=300 #hardcoded in dummy_drv
PREFERRED_HSYNC=50
RES="$W"x"$H"
HTOTAL=$(($W+3))
VTOTAL=$(($H+3))
CLOCK=$(($PREFERRED_HSYNC*$HTOTAL/1000))
if [ $CLOCK -gt $DUMMY_MAX_CLOCK ] ; then CLOCK=$DUMMY_MAX_CLOCK ; fi
MODELINE="$CLOCK $W $(($W+1)) $(($W+2)) $HTOTAL $H $(($H+1)) $(($H+2)) $VTOTAL"

HSYNC_START=$(($CLOCK*1000/$HTOTAL))
HSYNC_END=$((HSYNC_START+1))

VREFR_START=$(($CLOCK*1000000/$HTOTAL/$VTOTAL))
VREFR_END=$((VREFR_START+1))

sed -e  s/%MEM%/$MEM/ \
        -e  s/%DEPTH%/$DEPTH/ \
        -e  s/%MODELINE%/"$MODELINE"/ \
        -e  s/%HSYNC_START%/"$HSYNC_START"/ \
        -e  s/%HSYNC_END%/"$HSYNC_END"/ \
        -e  s/%VREFR_START%/"$VREFR_START"/ \
        -e  s/%VREFR_END%/"$VREFR_END"/ \
        -e  s/%RES%/QB$RES/ < /etc/X11/xorg-qubes.conf.template \
        > /etc/X11/xorg-qubes.conf

XSESSION="/etc/X11/xinit/xinitrc"
XORG="/usr/bin/X"
if [ -f /etc/X11/Xsession ]; then
    # Debian-based distro, set Xsession appropriately
    XSESSION="/etc/X11/Xsession qubes-session"
    # Debian installs Xorg without setuid root bit, with a setuid wrapper.
    # The wrapper is not useful for qubes, but it does not matter since
    # we can Xorg with qubes drivers without root. But we need to call
    # Xorg directly, not X (which is the wrapper).
    if [ -x /usr/lib/xorg/Xorg ]; then
        XORG="/usr/lib/xorg/Xorg"
    else
        XORG="/usr/bin/Xorg"
    fi
fi

# Make qubes input socket readable by user in case Xorg is not running as
# root (debian for example)
chown root:user /var/run/xf86-qubes-socket
chmod 770 /var/run/xf86-qubes-socket

export XDG_SEAT=seat0 XDG_VTNR=7 XDG_SESSION_CLASS=user

<<<<<<< HEAD
# Defaults value in case default-user value is not set
if [ ! -z "$(qubesdb-read /default-user)" ];then
    DEFAULT_USER=$(qubesdb-read /default-user)
else
    DEFAULT_USER="user"
fi

exec su -l "$DEFAULT_USER" -c "/usr/bin/xinit $XSESSION -- $XORG :0 -nolisten tcp vt07 -wr -config xorg-qubes.conf > ~/.xsession-errors 2>&1"
=======
exec su -l user -c "/usr/bin/xinit $XSESSION -- $XORG :0 -nolisten tcp vt07 -wr -config xorg-qubes.conf > ~/.xsession-errors 2>&1"
>>>>>>> parent of 315cb1d... Using the default_user available in qubesdb to start X
