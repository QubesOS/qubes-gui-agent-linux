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
if [ -f /etc/this_is_dvm ]; then
	# Skip system xinitrc to speed up DispVM startup
	exec su user -c '/usr/bin/xinit /usr/bin/qubes-session -- /usr/bin/X :0 -nolisten tcp vt07 -wr -config xorg-qubes.conf > ~/.xsession-errors 2>&1'
else
	exec su user -c '/usr/bin/xinit /etc/X11/xinit/xinitrc -- /usr/bin/X :0 -nolisten tcp vt07 -wr -config xorg-qubes.conf > ~/.xsession-errors 2>&1'
fi
