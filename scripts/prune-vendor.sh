#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB_DIR="$ROOT_DIR/vendor/libraop"

if [ ! -d "$LIB_DIR" ]; then
  echo "Nothing to prune: $LIB_DIR missing" >&2
  exit 0
fi

# Remove bulky, unused components.
rm -rf \
  "$LIB_DIR/bin" \
  "$LIB_DIR/targets" \
  "$LIB_DIR/doc" \
  "$LIB_DIR/libopenssl" \
  "$LIB_DIR/libpthreads4w" \
  "$LIB_DIR/build" \
  "$LIB_DIR/build.sh" \
  "$LIB_DIR/build.cmd"

# Keep only the ALAC codec from libcodecs.
if [ -d "$LIB_DIR/libcodecs" ]; then
  find "$LIB_DIR/libcodecs" -maxdepth 1 -type d \
    ! -name "libcodecs" \
    ! -name "alac" \
    -exec rm -rf {} +

  # Inside alac, keep only codec sources.
  if [ -d "$LIB_DIR/libcodecs/alac" ]; then
    find "$LIB_DIR/libcodecs/alac" -maxdepth 1 -type d \
      ! -name "alac" \
      ! -name "codec" \
      -exec rm -rf {} +
  fi
fi

# Trim curve25519 to source/include only (needed for pairing).
# Current build does not consume curve25519 sources directly; drop the whole tree to shrink tarball.
rm -rf "$LIB_DIR/curve25519"

# Remove any remaining git metadata.
find "$LIB_DIR" -name ".git" -type d -prune -exec rm -rf {} +
