#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB_DIR="$ROOT_DIR/vendor/libraop"

if [ ! -d "$LIB_DIR" ]; then
  echo "Nothing to prune: $LIB_DIR missing" >&2
  exit 0
fi

# Remove everything except the trees we actually compile.
find "$LIB_DIR" -maxdepth 1 -mindepth 1 -type d \
  ! -name src \
  ! -name crosstools \
  ! -name dmap-parser \
  ! -name libmdns \
  ! -name libcodecs \
  ! -name ".prepared" \
  -exec rm -rf {} +

# Keep only the ALAC codec from libcodecs.
if [ -d "$LIB_DIR/libcodecs" ]; then
  find "$LIB_DIR/libcodecs" -maxdepth 1 -mindepth 1 -type d \
    ! -name alac \
    -exec rm -rf {} +

  if [ -d "$LIB_DIR/libcodecs/alac" ]; then
    find "$LIB_DIR/libcodecs/alac" -maxdepth 1 -mindepth 1 -type d \
      ! -name codec \
      -exec rm -rf {} +
  fi
fi

# Keep only mdnssvc and mdnssd inside libmdns.
if [ -d "$LIB_DIR/libmdns" ]; then
  find "$LIB_DIR/libmdns" -maxdepth 1 -mindepth 1 -type d \
    ! -name mdnssvc \
    ! -name mdnssd \
    -exec rm -rf {} +
fi

# In dmap-parser, drop subdirectories (keep top-level sources).
if [ -d "$LIB_DIR/dmap-parser" ]; then
  find "$LIB_DIR/dmap-parser" -maxdepth 1 -mindepth 1 -type d -exec rm -rf {} +
fi

# Remove any remaining git metadata.
find "$LIB_DIR" -name ".git" -type d -prune -exec rm -rf {} +

# Remove any remaining git metadata.
find "$LIB_DIR" -name ".git" -type d -prune -exec rm -rf {} +
