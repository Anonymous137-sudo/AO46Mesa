#!/bin/sh
set -eu

# Keep the ABI filenames distinct while both resolve into one Mesa image. This
# preserves one glapi TLS dispatch table for EGL-created contexts and gl* calls.
ln -sf "$(basename "$1")" "$2"
