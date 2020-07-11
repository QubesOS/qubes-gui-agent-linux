# Maintainer: Frédéric Pierret <frederic.pierret@qubes-os.org>

EAPI=6

inherit git-r3 eutils multilib

MY_PV=${PV/_/-}
MY_P=${PN}-${MY_PV}

KEYWORDS="~amd64"
EGIT_REPO_URI="https://github.com/QubesOS/qubes-gui-agent-linux.git"
EGIT_COMMIT="v${PV}"
DESCRIPTION="The Qubes GUI Agent for AppVMs"
HOMEPAGE="http://www.qubes-os.org"
LICENSE="GPLv2"

SLOT="0"
IUSE=""

DEPEND="app-emulation/qubes-libvchan-xen \
        app-emulation/qubes-gui-common \
        x11-base/xorg-x11 \
        x11-libs/libXdamage \
        x11-apps/xinit \
        x11-libs/libXcomposite \
        dev-python/xcffib \
        media-libs/alsa-lib \
        media-sound/alsa-utils \
        media-sound/pulseaudio
        "
RDEPEND=""
PDEPEND=""

src_prepare() {
    einfo "Apply patch set"
    EPATCH_SUFFIX="patch" \
    EPATCH_FORCE="yes" \
    EPATCH_OPTS="-p1" \
    epatch "${FILESDIR}"

    default
}

src_compile() {
    pa_ver=$((pkg-config --modversion libpulse 2>/dev/null || echo 0.0) | cut -f 1 -d "-")

    rm -f pulse/pulsecore
    ln -s "pulsecore-$pa_ver" pulse/pulsecore

    # Bug fixes : /var/run/console depends on pam_console, which is Fedora specific
    # As a consequece, /var/run/console does not exists and qubes-gui-agent will always fail
    sed 's:ExecStartPre=/bin/touch:#ExecStartPre=/bin/touch:' -i appvm-scripts/qubes-gui-agent.service
    # Ensure that qubes-gui-agent starts after user autologin
    sed 's/After=\(.*\)qubes-misc-post.service/After=\1qubes-misc-post.service getty.target/' -i appvm-scripts/qubes-gui-agent.service

    myopt="${myopt} DESTDIR="${D}" SYSTEMD=1 BACKEND_VMM=xen LIBDIR=/usr/$(get_libdir)"
    emake ${myopt} appvm
}

src_install() {
    emake ${myopt} install-rh-agent
}

pkg_postinst() {
    systemctl enable qubes-gui-agent.service

    sed -i '/^autospawn/d' /etc/pulse/client.conf
    echo autospawn=no >> /etc/pulse/client.co
}

pkg_prerm() {
    systemctl disable qubes-gui-agent.service
}