#
# This is the SPEC file for creating binary and source RPMs for the VMs.
#
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


%{!?version: %define version %(cat version)}

Name:		qubes-gui-vm	
Version:	%{version}
Release:	1%{dist}
Summary:	The Qubes GUI Agent for AppVMs

Group:		Qubes
Vendor:		Invisible Things Lab
License:	GPL
URL:		http://www.qubes-os.org

Source:		.

%define pa_ver %(pkg-config --modversion libpulse || echo 0.0)

BuildRequires:	gcc
BuildRequires:	libX11-devel
BuildRequires:	libXcomposite-devel
BuildRequires:	libXdamage-devel
BuildRequires:	libXt-devel
BuildRequires:	libtool-ltdl-devel
BuildRequires:	libtool-ltdl-devel
BuildRequires:	pulseaudio-libs-devel >= 0.9.21, pulseaudio-libs-devel <= 3.0
BuildRequires:	xen-devel
BuildRequires:	xorg-x11-server-devel
BuildRequires:	qubes-core-libs-devel >= 1.6.1
BuildRequires:	qubes-core-libs
BuildRequires:	qubes-gui-common-devel
Requires:	qubes-core-vm >= 2.1.2
Requires:	xen-qubes-vm-essentials
Requires:	xorg-x11-drv-dummy

# The vchan sink needs .h files from pulseaudio sources
# that are not exported by any *-devel packages; thus they are internal and
# possible to change across version. They are copied to gui git. 
# It is possible that our code will work fine with any later pulseaudio
# version; but this needs to be verified for each pulseaudio version.
Requires:	pulseaudio = %{pa_ver}
AutoReq: 0

%define _builddir %(pwd)

%description
The Qubes GUI agent that needs to be installed in VM in order to provide the Qubes fancy GUI.

%prep
# we operate on the current directory, so no need to unpack anything
# symlink is to generate useful debuginfo packages
rm -f %{name}-%{version}
ln -sf . %{name}-%{version}
%setup -T -D

rm -f pulse/pulsecore
ln -s pulsecore-%{pa_ver} pulse/pulsecore

%build
#make clean
make appvm

%install
rm -rf $RPM_BUILD_ROOT
install -D gui-agent/qubes-gui $RPM_BUILD_ROOT/usr/bin/qubes-gui
install -D appvm-scripts/usrbin/qubes-session $RPM_BUILD_ROOT/usr/bin/qubes-session
install -D appvm-scripts/usrbin/qubes-run-xorg.sh $RPM_BUILD_ROOT/usr/bin/qubes-run-xorg.sh
install -D appvm-scripts/usrbin/qubes-xorg-wrapper.sh $RPM_BUILD_ROOT/usr/bin/qubes-xorg-wrapper.sh
install -D appvm-scripts/usrbin/qubes-change-keyboard-layout $RPM_BUILD_ROOT/usr/bin/qubes-change-keyboard-layout
install -D pulse/start-pulseaudio-with-vchan $RPM_BUILD_ROOT/usr/bin/start-pulseaudio-with-vchan
install -D pulse/qubes-default.pa $RPM_BUILD_ROOT/etc/pulse/qubes-default.pa
install -D pulse/module-vchan-sink.so $RPM_BUILD_ROOT/%{_libdir}/pulse-%{pa_ver}/modules/module-vchan-sink.so
install -D xf86-input-mfndev/src/.libs/qubes_drv.so $RPM_BUILD_ROOT/%{_libdir}/xorg/modules/drivers/qubes_drv.so
install -D relaxed-xf86ValidateModes/relaxed-xf86ValidateModes.so $RPM_BUILD_ROOT/%{_libdir}/relaxed-xf86ValidateModes.so
install -D appvm-scripts/etc/X11/xorg-qubes.conf.template $RPM_BUILD_ROOT/etc/X11/xorg-qubes.conf.template
install -D appvm-scripts/etc/init.d/qubes-gui-agent $RPM_BUILD_ROOT/etc/init.d/qubes-gui-agent
install -D appvm-scripts/etc/profile.d/qubes-gui.sh $RPM_BUILD_ROOT/etc/profile.d/qubes-gui.sh
install -D appvm-scripts/etc/profile.d/qubes-gui.csh $RPM_BUILD_ROOT/etc/profile.d/qubes-gui.csh
install -D appvm-scripts/etc/profile.d/qubes-session.sh $RPM_BUILD_ROOT/etc/profile.d/qubes-session.sh
install -D appvm-scripts/etc/sysconfig/desktop $RPM_BUILD_ROOT/etc/sysconfig/desktop
install -D appvm-scripts/etc/sysconfig/modules/qubes-u2mfn.modules $RPM_BUILD_ROOT/etc/sysconfig/modules/qubes-u2mfn.modules
install -D appvm-scripts/etc/X11/xinit/xinitrc.d/qubes-keymap.sh $RPM_BUILD_ROOT/etc/X11/xinit/xinitrc.d/qubes-keymap.sh
install -D appvm-scripts/etc/tmpfiles.d/qubes-pulseaudio.conf $RPM_BUILD_ROOT/usr/lib/tmpfiles.d/qubes-pulseaudio.conf
install -D appvm-scripts/etc/tmpfiles.d/qubes-session.conf $RPM_BUILD_ROOT/usr/lib/tmpfiles.d/qubes-session.conf
install -D appvm-scripts/etc/xdgautostart/qubes-pulseaudio.desktop $RPM_BUILD_ROOT/etc/xdg/autostart/qubes-pulseaudio.desktop
install -D appvm-scripts/qubes-gui-agent.service $RPM_BUILD_ROOT/lib/systemd/system/qubes-gui-agent.service
install -D appvm-scripts/qubes-gui-vm.gschema.override $RPM_BUILD_ROOT%{_datadir}/glib-2.0/schemas/20_qubes-gui-vm.gschema.override
install -d $RPM_BUILD_ROOT/var/log/qubes

%post
if [ -x /bin/systemctl ] && readlink /sbin/init | grep -q systemd; then
    /bin/systemctl enable qubes-gui-agent.service 2> /dev/null
    # For clean upgrades
    chkconfig qubes_gui off 2>/dev/null
else
    chkconfig qubes-gui-agent on
fi

sed -i '/^autospawn/d' /etc/pulse/client.conf
echo autospawn=no >> /etc/pulse/client.conf

%postun
if [ $1 -eq 0 ] ; then
fi

%preun
if [ "$1" = 0 ] ; then
	chkconfig qubes-gui-agent off
    [ -x /bin/systemctl ] && /bin/systemctl disable qubes-gui-agent.service
    /usr/bin/glib-compile-schemas %{_datadir}/glib-2.0/schemas &> /dev/null || :
fi

%posttrans
    /usr/bin/glib-compile-schemas %{_datadir}/glib-2.0/schemas &> /dev/null || :

%triggerin -- pulseaudio-libs

sed -i '/^autospawn/d' /etc/pulse/client.conf
echo autospawn=no >> /etc/pulse/client.conf

%clean
rm -rf $RPM_BUILD_ROOT
rm -f %{name}-%{version}


%files
%defattr(-,root,root,-)
/usr/bin/qubes-gui
/usr/bin/qubes-session
/usr/bin/qubes-run-xorg.sh
/usr/bin/qubes-xorg-wrapper.sh
/usr/bin/qubes-change-keyboard-layout
/usr/bin/start-pulseaudio-with-vchan
%{_libdir}/pulse-%{pa_ver}/modules/module-vchan-sink.so
%{_libdir}/xorg/modules/drivers/qubes_drv.so
%attr(4755,root,root) %{_libdir}/relaxed-xf86ValidateModes.so
%attr(0644,root,root) /etc/X11/xorg-qubes.conf.template
/etc/init.d/qubes-gui-agent
/etc/profile.d/qubes-gui.sh
/etc/profile.d/qubes-gui.csh
/etc/profile.d/qubes-session.sh
/etc/pulse/qubes-default.pa
/etc/xdg/autostart/qubes-pulseaudio.desktop
/etc/X11/xinit/xinitrc.d/qubes-keymap.sh
%config /etc/sysconfig/desktop
/etc/sysconfig/modules/qubes-u2mfn.modules
/lib/systemd/system/qubes-gui-agent.service
/usr/lib/tmpfiles.d/qubes-pulseaudio.conf
/usr/lib/tmpfiles.d/qubes-session.conf
%{_datadir}/glib-2.0/schemas/20_qubes-gui-vm.gschema.override
%dir /var/log/qubes
