#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB_DIR="$ROOT_DIR/vendor/libraop"
LIB_REPO="https://github.com/philippe44/libraop.git"
# Pin to a known-good commit for reproducible builds.
LIB_COMMIT="${LIB_COMMIT:-81c2182649da8645ac2a58b78e9f370c79a4165b}"

fetch_libraop() {
  echo "Fetching libraop sources from $LIB_REPO@$LIB_COMMIT"
  rm -rf "$LIB_DIR"
  git clone --depth 1 "$LIB_REPO" "$LIB_DIR"
  (
    cd "$LIB_DIR"
    git checkout "$LIB_COMMIT"
    git submodule update --init --recursive --depth 1
  )
  # Strip git metadata to keep the vendor tree clean.
  find "$LIB_DIR" -name ".git" -type d -prune -exec rm -rf {} +
}

patch_libraop_for_msvc() {
  local alac="$LIB_DIR/src/alac.c"
  if [ -f "$alac" ] && grep -q "#warning using generic count leading zeroes" "$alac"; then
    python3 - "$alac" <<'PY'
import sys
from pathlib import Path
alac = Path(sys.argv[1])
text = alac.read_text()
needle = "#else\n#warning using generic count leading zeroes. You may wish to write one for your CPU / compiler\nstatic int count_leading_zeros(int input)\n{\n"
replacement = "#else\n#ifdef _MSC_VER\n#pragma message(\"using generic count leading zeroes. You may wish to write one for your CPU / compiler\")\n#else\n#warning using generic count leading zeroes. You may wish to write one for your CPU / compiler\n#endif\nstatic int count_leading_zeros(int input)\n{\n"
if needle in text:
    alac.write_text(text.replace(needle, replacement))
PY
  fi
}

required_paths=(
  "$LIB_DIR/src/raop_server.c"
  "$LIB_DIR/src/raop_streamer.c"
  "$LIB_DIR/crosstools/src/cross_net.c"
  "$LIB_DIR/libmdns/mdnssvc/mdns.c"
  "$LIB_DIR/libmdns/mdnssd/mdnssd.c"
  "$LIB_DIR/libcodecs/alac/codec/ALACDecoder.cpp"
)

ensure_sources() {
  for path in "${required_paths[@]}"; do
    if [ ! -f "$path" ]; then
      return 1
    fi
  done
  return 0
}

if ! ensure_sources; then
  fetch_libraop
fi

if ! ensure_sources; then
  for path in "${required_paths[@]}"; do
    if [ ! -f "$path" ]; then
      echo "Missing vendored source: $path" >&2
    fi
  done
  exit 1
fi

patch_libraop_for_msvc
bash "$ROOT_DIR/scripts/prune-vendor.sh"

touch "$LIB_DIR/.prepared"
echo "libraop sources already vendored under $LIB_DIR"
