#!/usr/bin/sh

srcdir="$(dirname "$0")"
test -z "$srcdir" && srcdir=.

ORIGDIR="$PWD"
cd "$srcdir" || exit $?

autoreconf -v --install || exit 1
cd "$ORIGDIR" || exit $?

$srcdir/configure --enable-maintainer-mode "$@"
