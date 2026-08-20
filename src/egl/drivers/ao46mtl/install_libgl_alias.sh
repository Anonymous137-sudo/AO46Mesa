#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <libdir>" >&2
    exit 1
fi

prefix=${MESON_INSTALL_DESTDIR_PREFIX:-${MESON_INSTALL_PREFIX:?}}
libdir=$1
library_dir="$prefix/$libdir"

mkdir -p "$library_dir"
ln -sfn libEGL.1.dylib "$library_dir/libGL.dylib"
