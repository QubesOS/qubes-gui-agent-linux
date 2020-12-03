#!/bin/bash

set -ex

exit_updater() {
    local exit_code=$?
    rm -rf "$TMPDIR"
    rm -f "$LOCALDIR/${SRC_FILE:?}"
    if [ ${exit_code} -ge 1 ]; then
        echo "-> An error occurred while fetching latest pulsecore headers. Manual update is required"
    fi
    exit "${exit_code}"
}

LOCALDIR="$(readlink -f "$(dirname "$0")")"
BUILDERDIR="$(readlink -f "$LOCALDIR/../builder-rpm")"
TMPDIR="$(mktemp -d)"

if [ ! -d "$BUILDERDIR" ]; then
    echo "Cannot find qubes-builder-rpm. Exiting..."
    exit 1
fi

LATEST_FEDORA_RELEASE="$(git ls-remote --heads https://src.fedoraproject.org/rpms/fedora-release | grep -Po "refs/heads/f[0-9][1-9]*" | sed 's#refs/heads/f##g' | sort -g | tail -1)"
LATEST_FEDORA_VERREL="$(dnf -q repoquery pulseaudio --disablerepo=* --enablerepo=fedora --enablerepo=updates --releasever="$LATEST_FEDORA_RELEASE" | grep -Po "[0-9][1-9]*\.*[0-9]*\.*[0-9]*-[0-9]*" | sort -V | tail -1)"

LATEST_FEDORA_VERSION="$(echo "$LATEST_FEDORA_VERREL" | cut -d'-' -f1)"
LATEST_QUBES_VERSION="$(find "$LOCALDIR/pulse" -type d -name "pulsecore-*" | sed "s|$LOCALDIR/pulse/pulsecore-||" | sort -g | tail -1)"

SRC_RPM="pulseaudio-${LATEST_FEDORA_VERREL}.fc${LATEST_FEDORA_RELEASE}.src.rpm"
SRC_FILE="pulseaudio-${LATEST_FEDORA_VERSION}.tar.xz"

trap 'exit_updater' 0 1 2 3 6 15

if [ "${LATEST_QUBES_VERSION}" != "${LATEST_FEDORA_VERSION}" ] && [ ! -e "$LOCALDIR/pulse/pulsecore-${LATEST_FEDORA_VERSION}" ]; then
    "$BUILDERDIR/scripts/get_sources_from_srpm" "$LATEST_FEDORA_RELEASE" pulseaudio "$SRC_RPM" "$SRC_FILE"

    # remove unwanted files
    cd "$TMPDIR"
    tar -xf "$LOCALDIR/$SRC_FILE"
    find "pulseaudio-$LATEST_FEDORA_VERSION/src/pulsecore" -type f ! -regex '.*\.h$' -exec rm -f {} \;
    rm -f "pulseaudio-$LATEST_FEDORA_VERSION/src/Makefile"

    # copy to qubes-gui-agent
    cp -r "pulseaudio-$LATEST_FEDORA_VERSION/src/pulsecore" "$LOCALDIR/pulse/pulsecore-$LATEST_FEDORA_VERSION"
fi
