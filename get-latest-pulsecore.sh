#!/bin/bash

set -euxo pipefail

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

LATEST_REPO_VERSION=$(
  git ls-remote --exit-code --refs --tags --sort="v:refname" "$REPO_URL" |
  head -c $((1 << 16)) |
  sed -nE $'$ s%^[0-9a-f]{40}\trefs/tags/v([0-9]+(\\.[0-9]{1,5}){1,2})$%\\1%p'
)
LATEST_QUBES_VERSION="$(find "$LOCALDIR/pulse" -type d -name "pulsecore-*" | sed "s|$LOCALDIR/pulse/pulsecore-||" | sort -g | tail -1)"

trap 'exit_updater' 0 1 2 3 6 15

if [ "${LATEST_QUBES_VERSION}" != "${LATEST_REPO_VERSION}" ] && [ ! -e "$LOCALDIR/pulse/pulsecore-${LATEST_REPO_VERSION}" ]; then
    cd "$TMPDIR"
    mkdir gnupg-tmp gnupg git
    export "GNUPGHOME=$PWD/gnupg"
    cd git

    git clone --no-checkout --depth 1 --branch "v$LATEST_REPO_VERSION" "$REPO_URL" .

    trusted_signers=(
        52DFA7B8BAC74687C8A88EF48165E3D1987E2132
        B61E1D411D57BD16F11536162477064CE8B9F3BD
    )
    # Import keys of repo taggers
    for key in "${trusted_signers[@]}"; do
        echo "$key:6:" | gpg --import-ownertrust
        for i in keyserver.ubuntu.com keys.openpgp.org pgp.mit.edu keyserver.pgp.com; do
            sq keyserver --server "$i" get --binary -- "0x$key" && break
        done
    done | gpg --homedir=../gnupg-tmp --import --no-armor
    gpg --homedir=../gnupg-tmp --export -- "${trusted_signers[@]}" | gpg --import --no-armor

    for key in "${trusted_signers[@]}"; do
        echo "$key:6:"
    done | gpg --import-ownertrust

    tag_to_verify="refs/tags/v$LATEST_REPO_VERSION"

    # Verify integrity
    git -c gpg.openpgp.program=gpg -c gpg.minTrustLevel=ultimate verify-tag "$tag_to_verify" || exit
    git checkout "$tag_to_verify^{commit}"

    # remove unwanted files
    find "src/pulsecore" -type f ! -regex '.*\.h$' -exec rm -f {} \;

    # copy to qubes-gui-agent
    cp -r "src/pulsecore" "$LOCALDIR/pulse/pulsecore-$LATEST_REPO_VERSION"
fi
