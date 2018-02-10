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

RPMS_DIR=rpm/
VERSION := $(shell cat version)

DIST_DOM0 ?= fc13

LIBDIR ?= /usr/lib64
USRLIBDIR ?= /usr/lib
SYSLIBDIR ?= /lib
DATADIR ?= /usr/share
PA_VER ?= $(shell pkg-config --modversion libpulse | cut -d "-" -f 1 || echo 0.0)

help:
	@echo "Qubes GUI main Makefile:" ;\
	    echo "make rpms                 <--- make all rpms and sign them";\
	    echo "make rpms-dom0            <--- create binary rpms for dom0"; \
	    echo "make rpms-vm              <--- create binary rpms for appvm"; \
	    echo; \
	    echo "make clean                <--- clean all the binary files";\
	    echo "make update-repo-current  <-- copy newly generated rpms to qubes yum repo";\
	    echo "make update-repo-current-testing <-- same, but for -current-testing repo";\
	    echo "make update-repo-unstable <-- same, but to -testing repo";\
	    echo "make update-repo-installer -- copy dom0 rpms to installer repo"
	    @exit 0;

appvm: gui-agent/qubes-gui xf86-input-mfndev/src/.libs/qubes_drv.so \
	xf86-video-dummy/src/.libs/dummyqbs_drv.so pulse/module-vchan-sink.so

gui-agent/qubes-gui:
	(cd gui-agent; $(MAKE))

xf86-input-mfndev/src/.libs/qubes_drv.so:
	(cd xf86-input-mfndev && ./bootstrap && ./configure && make LDFLAGS=-lu2mfn)

xf86-video-dummy/src/.libs/dummyqbs_drv.so:
	(cd xf86-video-dummy && ./autogen.sh && make)

pulse/module-vchan-sink.so:
	rm -f pulse/pulsecore
	ln -s pulsecore-$(PA_VER) pulse/pulsecore
	$(MAKE) -C pulse module-vchan-sink.so

rpms: rpms-dom0 rpms-vm
	rpm --addsign rpm/x86_64/*$(VERSION)*.rpm
	(if [ -d rpm/i686 ] ; then rpm --addsign rpm/i686/*$(VERSION)*.rpm; fi)

rpms-vm:
	rpmbuild --define "_rpmdir rpm/" -bb rpm_spec/gui-vm.spec

tar:
	git archive --format=tar --prefix=qubes-gui/ HEAD -o qubes-gui.tar

clean:
	(cd common && $(MAKE) clean)
	(cd gui-agent && $(MAKE) clean)
	(cd gui-common && $(MAKE) clean)
	$(MAKE) -C pulse clean
	(cd xf86-input-mfndev; if [ -e Makefile ] ; then \
		$(MAKE) distclean; fi; ./bootstrap --clean || echo )


install: install-rh-agent install-pulseaudio

install-rh-agent: appvm install-common
	install -D appvm-scripts/etc/init.d/qubes-gui-agent \
		$(DESTDIR)/etc/init.d/qubes-gui-agent
	install -m 0644 -D appvm-scripts/qubes-gui-agent.service \
		$(DESTDIR)/$(SYSLIBDIR)/systemd/system/qubes-gui-agent.service
	install -m 0644 -D appvm-scripts/etc/sysconfig/desktop \
		$(DESTDIR)/etc/sysconfig/desktop
	install -D appvm-scripts/etc/sysconfig/modules/qubes-u2mfn.modules \
		$(DESTDIR)/etc/sysconfig/modules/qubes-u2mfn.modules
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/qubes-keymap.sh \
		$(DESTDIR)/etc/X11/xinit/xinitrc.d/qubes-keymap.sh
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/20qt-x11-no-mitshm.sh \
		$(DESTDIR)/etc/X11/xinit/xinitrc.d/20qt-x11-no-mitshm.sh
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/20qt-gnome-desktop-session-id.sh \
		$(DESTDIR)/etc/X11/xinit/xinitrc.d/20qt-gnome-desktop-session-id.sh
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/50-xfce-desktop.sh \
                $(DESTDIR)/etc/X11/xinit/xinitrc.d/50-xfce-desktop.sh
	install -m 0644 -D appvm-scripts/etc/X11/Xwrapper.config \
		$(DESTDIR)/etc/X11/Xwrapper.config

install-debian: appvm install-common install-pulseaudio
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/qubes-keymap.sh \
		$(DESTDIR)/etc/X11/Xsession.d/90qubes-keymap
	install -d $(DESTDIR)/etc/X11/Xsession.d
	install appvm-scripts/etc/X11/Xsession.d/* $(DESTDIR)/etc/X11/Xsession.d/
	install -d $(DESTDIR)/etc/xdg
	install -m 0644 appvm-scripts/etc/xdg-debian/* $(DESTDIR)/etc/xdg
	install -m 0644 -D appvm-scripts/qubes-gui-agent.service \
		$(DESTDIR)/$(SYSLIBDIR)/systemd/system/qubes-gui-agent.service

install-pulseaudio:
	install -D pulse/start-pulseaudio-with-vchan \
		$(DESTDIR)/usr/bin/start-pulseaudio-with-vchan
	install -m 0644 -D pulse/qubes-default.pa \
		$(DESTDIR)/etc/pulse/qubes-default.pa
	install -D pulse/module-vchan-sink.so \
		$(DESTDIR)$(LIBDIR)/pulse-$(PA_VER)/modules/module-vchan-sink.so
	install -m 0644 -D appvm-scripts/etc/tmpfiles.d/qubes-pulseaudio.conf \
		$(DESTDIR)/$(USRLIBDIR)/tmpfiles.d/qubes-pulseaudio.conf
	install -m 0644 -D appvm-scripts/etc/xdgautostart/qubes-pulseaudio.desktop \
		$(DESTDIR)/etc/xdg/autostart/qubes-pulseaudio.desktop

install-common:
	install -D gui-agent/qubes-gui $(DESTDIR)/usr/bin/qubes-gui
	install -D appvm-scripts/usrbin/qubes-session \
		$(DESTDIR)/usr/bin/qubes-session
	install -D appvm-scripts/usrbin/qubes-run-xorg.sh \
		$(DESTDIR)/usr/bin/qubes-run-xorg.sh
	install -D appvm-scripts/usrbin/qubes-change-keyboard-layout \
		$(DESTDIR)/usr/bin/qubes-change-keyboard-layout
	install -D appvm-scripts/usrbin/qubes-set-monitor-layout \
		$(DESTDIR)/usr/bin/qubes-set-monitor-layout
	install -D xf86-input-mfndev/src/.libs/qubes_drv.so \
		$(DESTDIR)$(LIBDIR)/xorg/modules/drivers/qubes_drv.so
	install -D xf86-video-dummy/src/.libs/dummyqbs_drv.so \
		$(DESTDIR)$(LIBDIR)/xorg/modules/drivers/dummyqbs_drv.so
	install -m 0644 -D appvm-scripts/etc/X11/xorg-qubes.conf.template \
		$(DESTDIR)/etc/X11/xorg-qubes.conf.template
	install -m 0644 -D appvm-scripts/etc/profile.d/qubes-gui.sh \
		$(DESTDIR)/etc/profile.d/qubes-gui.sh
	install -m 0644 -D appvm-scripts/etc/profile.d/qubes-gui.csh \
		$(DESTDIR)/etc/profile.d/qubes-gui.csh
	install -m 0644 -D appvm-scripts/etc/profile.d/qubes-session.sh \
		$(DESTDIR)/etc/profile.d/qubes-session.sh
	install -m 0644 -D appvm-scripts/etc/tmpfiles.d/qubes-session.conf \
		$(DESTDIR)/$(USRLIBDIR)/tmpfiles.d/qubes-session.conf
	install -m 0644 -D appvm-scripts/etc/securitylimits.d/90-qubes-gui.conf \
		$(DESTDIR)/etc/security/limits.d/90-qubes-gui.conf
ifneq ($(shell lsb_release -is), Ubuntu)
	install -m 0644 -D appvm-scripts/etc/xdg/Trolltech.conf \
		$(DESTDIR)/etc/xdg/Trolltech.conf
endif
	install -m 0644 -D appvm-scripts/qubes-gui-vm.gschema.override \
		$(DESTDIR)$(DATADIR)/glib-2.0/schemas/20_qubes-gui-vm.gschema.override
	install -m 0644 -D appvm-scripts/etc/qubes-rpc/qubes.SetMonitorLayout \
		$(DESTDIR)/etc/qubes-rpc/qubes.SetMonitorLayout
	install -D window-icon-updater/icon-sender \
		$(DESTDIR)/usr/lib/qubes/icon-sender
	install -m 0644 -D window-icon-updater/qubes-icon-sender.desktop \
		$(DESTDIR)/etc/xdg/autostart/qubes-icon-sender.desktop
	install -D -m 0644 appvm-scripts/usr/lib/sysctl.d/30-qubes-gui-agent.conf \
		$(DESTDIR)/usr/lib/sysctl.d/30-qubes-gui-agent.conf
	install -d $(DESTDIR)/var/log/qubes
