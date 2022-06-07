#!/bin/bash

set -ex

exit_updater() {
    local exit_code=$?
    rm -rf "$TMPDIR"
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

REPO_URL=https://gitlab.freedesktop.org/pulseaudio/pulseaudio.git

LATEST_REPO_VERSION="$(git ls-remote --exit-code --refs --tags --sort="v:refname" "$REPO_URL" '*.*' | tail -n1 | cut -d/ -f3 | sed 's/^v//')"
LATEST_QUBES_VERSION="$(find "$LOCALDIR/pulse" -type d -name "pulsecore-*" | sed "s|$LOCALDIR/pulse/pulsecore-||" | sort -g | tail -1)"

trap 'exit_updater' 0 1 2 3 6 15

if [ "${LATEST_QUBES_VERSION}" != "${LATEST_REPO_VERSION}" ] && [ ! -e "$LOCALDIR/pulse/pulsecore-${LATEST_REPO_VERSION}" ]; then
    cd "$TMPDIR"

    git clone --depth 1 --branch "v$LATEST_REPO_VERSION" "$REPO_URL" .

    # Import keys of repo taggers
    for key in \
        52DFA7B8BAC74687C8A88EF48165E3D1987E2132 \
        B61E1D411D57BD16F11536162477064CE8B9F3BD; do
        gpg --batch --keyserver keyserver.ubuntu.com --recv-keys "$key" ||
            gpg --batch --keyserver keys.openpgp.org --recv-keys "$key" ||
            gpg --batch --keyserver pgp.mit.edu --recv-keys "$key" ||
            gpg --batch --keyserver keyserver.pgp.com --recv-keys "$key" ||
            gpg --batch --keyserver ha.pool.sks-keyservers.net --recv-keys "$key"
    done

    # Verify integrity
    git tag -v "$(git describe)"

    # remove unwanted files
    find "src/pulsecore" -type f ! -regex '.*\.h$' -exec rm -f {} \;

    # copy to qubes-gui-agent
    cp -r "src/pulsecore" "$LOCALDIR/pulse/pulsecore-$LATEST_REPO_VERSION"
fi
