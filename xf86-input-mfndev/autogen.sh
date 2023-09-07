#! /bin/sh

set -x

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`
cd $srcdir

autoupdate -v -f || exit 1
autoreconf -v --install || exit 1
cd $ORIGDIR || exit $?

$srcdir/configure --enable-maintainer-mode "$@"
