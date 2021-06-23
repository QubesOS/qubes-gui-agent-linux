#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
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

VERSION := $(shell cat version)
LIBDIR ?= /usr/lib64
USRLIBDIR ?= /usr/lib
SYSLIBDIR ?= /lib
DATADIR ?= /usr/share
PA_VER_FULL ?= $(shell pkg-config --modversion libpulse | cut -d "-" -f 1 || echo 0.0)
PA_VER_MAJOR_MINOR ?= $(shell echo $(PA_VER_FULL) | cut -d "." -f 1,2)

help:
	@echo "Qubes GUI main Makefile:" ;\
	    echo; \
	    echo "make clean                <--- clean all the binary files";\
	    @exit 0;

appvm: gui-agent/qubes-gui gui-common/qubes-gui-runuser \
	xf86-input-mfndev/src/.libs/qubes_drv.so \
	xf86-video-dummy/src/.libs/dummyqbs_drv.so pulse/module-vchan-sink.so \
	xf86-qubes-common/libxf86-qubes-common.so

gui-agent/qubes-gui:
	$(MAKE) -C gui-agent

gui-common/qubes-gui-runuser:
	$(MAKE) -C gui-common

xf86-input-mfndev/src/.libs/qubes_drv.so: xf86-qubes-common/libxf86-qubes-common.so
	(cd xf86-input-mfndev && ./autogen.sh && ./configure)
	$(MAKE) -C xf86-input-mfndev

xf86-video-dummy/src/.libs/dummyqbs_drv.so: xf86-qubes-common/libxf86-qubes-common.so
	(cd xf86-video-dummy && ./autogen.sh)
	$(MAKE) -C xf86-video-dummy

pulse/module-vchan-sink.so:
	rm -f pulse/pulsecore
	ln -s pulsecore-$(PA_VER_FULL) pulse/pulsecore
	$(MAKE) -C pulse module-vchan-sink.so

xf86-qubes-common/libxf86-qubes-common.so:
	$(MAKE) -C xf86-qubes-common libxf86-qubes-common.so

tar:
	git archive --format=tar --prefix=qubes-gui/ HEAD -o qubes-gui.tar

clean:
	(cd common && $(MAKE) clean)
	(cd gui-agent && $(MAKE) clean)
	(cd gui-common && $(MAKE) clean)
	$(MAKE) -C pulse clean
	$(MAKE) -C xf86-qubes-common clean
	(cd xf86-input-mfndev; if [ -e Makefile ] ; then \
		$(MAKE) distclean; fi; ./bootstrap --clean || echo )
	rm -rf debian/changelog.*
	rm -rf pkgs


install: install-rh-agent install-pulseaudio

install-rh-agent: appvm install-common install-systemd
	install -m 0644 -D appvm-scripts/etc/sysconfig/desktop \
		$(DESTDIR)/etc/sysconfig/desktop
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/20qt-x11-no-mitshm.sh \
		$(DESTDIR)/etc/X11/xinit/xinitrc.d/20qt-x11-no-mitshm.sh
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/20qt-gnome-desktop-session-id.sh \
		$(DESTDIR)/etc/X11/xinit/xinitrc.d/20qt-gnome-desktop-session-id.sh
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/50guivm-windows-prefix.sh \
		$(DESTDIR)/etc/X11/xinit/xinitrc.d/50guivm-windows-prefix.sh
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/60xfce-desktop.sh \
		$(DESTDIR)/etc/X11/xinit/xinitrc.d/60xfce-desktop.sh

install-debian: appvm install-common install-pulseaudio install-systemd
	install -d $(DESTDIR)/etc/X11/Xsession.d
	install -m 0644 appvm-scripts/etc/X11/Xsession.d/* $(DESTDIR)/etc/X11/Xsession.d/
	install -d $(DESTDIR)/etc/xdg
	install -m 0644 appvm-scripts/etc/xdg-debian/* $(DESTDIR)/etc/xdg

install-pulseaudio:
	install -D pulse/start-pulseaudio-with-vchan \
		$(DESTDIR)/usr/bin/start-pulseaudio-with-vchan
	install -m 0644 -D pulse/qubes-default.pa \
		$(DESTDIR)/etc/pulse/qubes-default.pa
ifneq ($(shell lsb_release -is), Ubuntu)
	install -D pulse/module-vchan-sink.so \
		$(DESTDIR)$(LIBDIR)/pulse-$(PA_VER_MAJOR_MINOR)/modules/module-vchan-sink.so
else
	install -D pulse/module-vchan-sink.so \
		$(DESTDIR)$(LIBDIR)/pulse-$(PA_VER_FULL)/modules/module-vchan-sink.so
endif
	install -m 0644 -D appvm-scripts/etc/tmpfiles.d/qubes-pulseaudio.conf \
		$(DESTDIR)/$(USRLIBDIR)/tmpfiles.d/qubes-pulseaudio.conf
	install -m 0644 -D appvm-scripts/etc/xdgautostart/qubes-pulseaudio.desktop \
		$(DESTDIR)/etc/xdg/autostart/qubes-pulseaudio.desktop

install-systemd:
	install -m 0644 -D appvm-scripts/qubes-gui-agent.service \
		$(DESTDIR)/$(SYSLIBDIR)/systemd/system/qubes-gui-agent.service

install-common:
	install -D gui-agent/qubes-gui $(DESTDIR)/usr/bin/qubes-gui
	install -D gui-common/qubes-gui-runuser $(DESTDIR)/usr/bin/qubes-gui-runuser
	install -d $(DESTDIR)/etc/qubes/post-install.d
	install -m 0755 appvm-scripts/etc/qubes/post-install.d/20-qubes-guivm-gui-agent.sh \
                $(DESTDIR)/etc/qubes/post-install.d/20-qubes-guivm-gui-agent.sh
	install -D appvm-scripts/usrbin/qubes-session \
		$(DESTDIR)/usr/bin/qubes-session
	install -D appvm-scripts/usrbin/qubes-run-xorg \
		$(DESTDIR)/usr/bin/qubes-run-xorg
	install -D appvm-scripts/usrbin/qubes-run-xephyr \
		$(DESTDIR)/usr/bin/qubes-run-xephyr
	install -D appvm-scripts/usrbin/qubes-start-xephyr \
		$(DESTDIR)/usr/bin/qubes-start-xephyr
	install -D appvm-scripts/usrbin/qubes-run-x11vnc \
		$(DESTDIR)/usr/bin/qubes-run-x11vnc
	install -D appvm-scripts/usrbin/qubes-change-keyboard-layout \
		$(DESTDIR)/usr/bin/qubes-change-keyboard-layout
	install -D appvm-scripts/usrbin/qubes-set-monitor-layout \
		$(DESTDIR)/usr/bin/qubes-set-monitor-layout
	install -D xf86-qubes-common/libxf86-qubes-common.so \
		$(DESTDIR)$(LIBDIR)/libxf86-qubes-common.so
	install -D xf86-input-mfndev/src/.libs/qubes_drv.so \
		$(DESTDIR)$(LIBDIR)/xorg/modules/drivers/qubes_drv.so
	install -D xf86-video-dummy/src/.libs/dummyqbs_drv.so \
		$(DESTDIR)$(LIBDIR)/xorg/modules/drivers/dummyqbs_drv.so
	install -m 0644 -D appvm-scripts/etc/X11/xorg-qubes.conf.template \
		$(DESTDIR)/etc/X11/xorg-qubes.conf.template
	install -m 0644 -D appvm-scripts/etc/X11/xorg-qubes-x11vnc.conf.template \
		$(DESTDIR)/etc/X11/xorg-qubes-x11vnc.conf.template
	install -m 0644 -D appvm-scripts/etc/systemd/system/lightdm.service.d/qubes-guivm-vnc.conf \
		$(DESTDIR)/etc/systemd/system/lightdm.service.d/qubes-guivm-vnc.conf
	install -m 0644 -D appvm-scripts/etc/profile.d/qubes-gui.sh \
		$(DESTDIR)/etc/profile.d/qubes-gui.sh
	install -m 0644 -D appvm-scripts/etc/profile.d/qubes-gui.csh \
		$(DESTDIR)/etc/profile.d/qubes-gui.csh
	install -m 0644 -D appvm-scripts/etc/securitylimits.d/90-qubes-gui.conf \
		$(DESTDIR)/etc/security/limits.d/90-qubes-gui.conf
ifneq ($(shell lsb_release -is), Ubuntu)
	install -m 0644 -D appvm-scripts/etc/xdg/Trolltech.conf \
		$(DESTDIR)/etc/xdg/Trolltech.conf
endif
	install -m 0644 -D appvm-scripts/qubes-gui-vm.gschema.override \
		$(DESTDIR)$(DATADIR)/glib-2.0/schemas/20_qubes-gui-vm.gschema.override
	install -d $(DESTDIR)/etc/qubes-rpc
	ln -s ../../usr/bin/qubes-set-monitor-layout \
		$(DESTDIR)/etc/qubes-rpc/qubes.SetMonitorLayout
	ln -s ../../usr/bin/qubes-start-xephyr \
		$(DESTDIR)/etc/qubes-rpc/qubes.GuiVMSession
	install -D window-icon-updater/icon-sender \
		$(DESTDIR)/usr/lib/qubes/icon-sender
	install -m 0644 -D window-icon-updater/qubes-icon-sender.desktop \
		$(DESTDIR)/etc/xdg/autostart/qubes-icon-sender.desktop
	install -m 0644 -D appvm-scripts/etc/xdgautostart/qubes-qrexec-fork-server.desktop \
		$(DESTDIR)/etc/xdg/autostart/qubes-qrexec-fork-server.desktop
	install -m 0644 -D appvm-scripts/etc/xdgautostart/qubes-keymap.desktop \
		$(DESTDIR)/etc/xdg/autostart/qubes-keymap.desktop
	install -D -m 0644 appvm-scripts/usr/lib/sysctl.d/30-qubes-gui-agent.conf \
		$(DESTDIR)/usr/lib/sysctl.d/30-qubes-gui-agent.conf
	install -D -m 0644 appvm-scripts/lib/udev/rules.d/70-master-of-seat.rules \
		$(DESTDIR)/$(SYSLIBDIR)/udev/rules.d/70-master-of-seat.rules
	install -D appvm-scripts/usr/lib/qubes/qubes-gui-agent-pre.sh \
		$(DESTDIR)/usr/lib/qubes/qubes-gui-agent-pre.sh
	install -D appvm-scripts/usr/lib/qubes/qubes-keymap.sh \
		$(DESTDIR)/usr/lib/qubes/qubes-keymap.sh
ifeq ($(shell lsb_release -is), Debian)
	install -D -m 0644 appvm-scripts/etc/pam.d/qubes-gui-agent.debian \
		$(DESTDIR)/etc/pam.d/qubes-gui-agent
else ifeq ($(shell lsb_release -is), Ubuntu)
	install -D -m 0644 appvm-scripts/etc/pam.d/qubes-gui-agent.debian \
		$(DESTDIR)/etc/pam.d/qubes-gui-agent
else ifeq ($(shell lsb_release -is), Arch)
	install -D -m 0644 appvm-scripts/etc/pam.d/qubes-gui-agent.archlinux \
		$(DESTDIR)/etc/pam.d/qubes-gui-agent
else
	install -D -m 0644 appvm-scripts/etc/pam.d/qubes-gui-agent \
		$(DESTDIR)/etc/pam.d/qubes-gui-agent
endif
	install -d $(DESTDIR)/var/log/qubes
