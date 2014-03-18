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

pulse/pacat-simple-vchan:
	$(MAKE) -C pulse pacat-simple-vchan

gui-agent/qubes-gui:
	(cd gui-agent; $(MAKE))

xf86-input-mfndev/src/.libs/qubes_drv.so:
	(cd xf86-input-mfndev && ./bootstrap && ./configure && make LDFLAGS=-lu2mfn)

xf86-video-dummy/src/.libs/dummyqbs_drv.so:
	(cd xf86-video-dummy && ./autogen.sh && make)

pulse/module-vchan-sink.so:
	$(MAKE) -C pulse module-vchan-sink.so

rpms: rpms-dom0 rpms-vm
	rpm --addsign rpm/x86_64/*$(VERSION)*.rpm
	(if [ -d rpm/i686 ] ; then rpm --addsign rpm/i686/*$(VERSION)*.rpm; fi)

rpms-vm:
	rpmbuild --define "_rpmdir rpm/" -bb rpm_spec/gui-vm.spec

rpms-dom0:
	rpmbuild --define "_rpmdir rpm/" -bb rpm_spec/gui-dom0.spec

tar:
	git archive --format=tar --prefix=qubes-gui/ HEAD -o qubes-gui.tar

clean:
	(cd common && $(MAKE) clean)
	(cd gui-agent && $(MAKE) clean)
	(cd gui-common && $(MAKE) clean)
	$(MAKE) -C pulse clean
	(cd xf86-input-mfndev; if [ -e Makefile ] ; then $(MAKE) distclean; fi; ./bootstrap --clean || echo )

install: appvm
	install -D gui-agent/qubes-gui $(DESTDIR)/usr/bin/qubes-gui
	install -D appvm-scripts/usrbin/qubes-session $(DESTDIR)/usr/bin/qubes-session
	install -D appvm-scripts/usrbin/qubes-run-xorg.sh $(DESTDIR)/usr/bin/qubes-run-xorg.sh
	install -D appvm-scripts/usrbin/qubes-change-keyboard-layout $(DESTDIR)/usr/bin/qubes-change-keyboard-layout
	install -D appvm-scripts/usrbin/qubes-set-monitor-layout $(DESTDIR)/usr/bin/qubes-set-monitor-layout
	install -D pulse/start-pulseaudio-with-vchan $(DESTDIR)/usr/bin/start-pulseaudio-with-vchan
	install -D pulse/qubes-default.pa $(DESTDIR)/etc/pulse/qubes-default.pa
	install -D pulse/module-vchan-sink.so $(DESTDIR)$(LIBDIR)/pulse-$(PA_VER)/modules/module-vchan-sink.so
	install -D xf86-input-mfndev/src/.libs/qubes_drv.so $(DESTDIR)$(LIBDIR)/xorg/modules/drivers/qubes_drv.so
	install -D xf86-video-dummy/src/.libs/dummyqbs_drv.so $(DESTDIR)$(LIBDIR)/xorg/modules/drivers/dummyqbs_drv.so
	install -D appvm-scripts/etc/X11/xorg-qubes.conf.template $(DESTDIR)/etc/X11/xorg-qubes.conf.template
	install -D appvm-scripts/etc/init.d/qubes-gui-agent $(DESTDIR)/etc/init.d/qubes-gui-agent
	install -D appvm-scripts/etc/profile.d/qubes-gui.sh $(DESTDIR)/etc/profile.d/qubes-gui.sh
	install -D appvm-scripts/etc/profile.d/qubes-gui.csh $(DESTDIR)/etc/profile.d/qubes-gui.csh
	install -D appvm-scripts/etc/profile.d/qubes-session.sh $(DESTDIR)/etc/profile.d/qubes-session.sh
	install -D appvm-scripts/etc/sysconfig/desktop $(DESTDIR)/etc/sysconfig/desktop
	install -D appvm-scripts/etc/sysconfig/modules/qubes-u2mfn.modules $(DESTDIR)/etc/sysconfig/modules/qubes-u2mfn.modules
	install -D appvm-scripts/etc/X11/xinit/xinitrc.d/qubes-keymap.sh $(DESTDIR)/etc/X11/xinit/xinitrc.d/qubes-keymap.sh
	install -D appvm-scripts/etc/tmpfiles.d/qubes-pulseaudio.conf $(DESTDIR)/usr/lib/tmpfiles.d/qubes-pulseaudio.conf
	install -D appvm-scripts/etc/tmpfiles.d/qubes-session.conf $(DESTDIR)/usr/lib/tmpfiles.d/qubes-session.conf
	install -D appvm-scripts/etc/xdgautostart/qubes-pulseaudio.desktop $(DESTDIR)/etc/xdg/autostart/qubes-pulseaudio.desktop
	install -D appvm-scripts/qubes-gui-agent.service $(DESTDIR)/lib/systemd/system/qubes-gui-agent.service
	install -D appvm-scripts/qubes-gui-vm.gschema.override $(DESTDIR)$(DATADIR)/glib-2.0/schemas/20_qubes-gui-vm.gschema.override
	install -m 0644 -D appvm-scripts/etc/qubes-rpc/qubes.SetMonitorLayout $(DESTDIR)/etc/qubes-rpc/qubes.SetMonitorLayout
	install -d $(DESTDIR)/var/log/qubes
