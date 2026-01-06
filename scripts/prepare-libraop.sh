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

patch_cross_log() {
  local hdr="$LIB_DIR/crosstools/src/cross_log.h"
  local src="$LIB_DIR/crosstools/src/cross_log.c"
  if [ -f "$hdr" ] && ! grep -q "cross_set_logger" "$hdr"; then
    python3 - "$hdr" <<'PY'
import sys
from pathlib import Path
hdr = Path(sys.argv[1])
text = hdr.read_text()
text = text.replace(
    "void logprint(const char *fmt, ...);\nlog_level debug2level",
    "void logprint_ex(log_level *current_level, const char *func, int line, const char *fmt, ...);\nlog_level debug2level",
)
text = text.replace(
    "#define LOG_ERROR(fmt, ...)  if (*loglevel >= lERROR)  logprint(\"%s %s:%d \" fmt \"\\n\", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)\n"
    "#define LOG_WARN(fmt, ...)   if (*loglevel >= lWARN)  logprint(\"%s %s:%d \" fmt \"\\n\", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)\n"
    "#define LOG_INFO(fmt, ...)   if (*loglevel >= lINFO)  logprint(\"%s %s:%d \" fmt \"\\n\", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)\n"
    "#define LOG_DEBUG(fmt, ...)  if (*loglevel >= lDEBUG) logprint(\"%s %s:%d \" fmt \"\\n\", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)\n"
    "#define LOG_SDEBUG(fmt, ...) if (*loglevel >= lSDEBUG) logprint(\"%s %s:%d \" fmt \"\\n\", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)\n",
    "typedef void (*log_sink_fn)(const char* source, const char* line, log_level level);\n"
    "void cross_set_logger(log_sink_fn fn);\n"
    "void cross_set_levels(log_level util_level, log_level raop_level);\n\n"
    "#define LOG_ERROR(fmt, ...)  if (*loglevel >= lERROR)  logprint_ex(loglevel, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)\n"
    "#define LOG_WARN(fmt, ...)   if (*loglevel >= lWARN)   logprint_ex(loglevel, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)\n"
    "#define LOG_INFO(fmt, ...)   if (*loglevel >= lINFO)   logprint_ex(loglevel, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)\n"
    "#define LOG_DEBUG(fmt, ...)  if (*loglevel >= lDEBUG)  logprint_ex(loglevel, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)\n"
    "#define LOG_SDEBUG(fmt, ...) if (*loglevel >= lSDEBUG) logprint_ex(loglevel, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)\n",
)
hdr.write_text(text)
PY
  fi

  if [ -f "$src" ] && ! grep -q "cross_set_logger" "$src"; then
    python3 - "$src" <<'PY'
import sys
from pathlib import Path
src = Path(sys.argv[1])
src.write_text(r'''/*
 * Logging utilities
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *  Philippe, philippe_44@outlook.com
 *
 * See LICENSE
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <sys/time.h>
#endif

#include "cross_log.h"

static log_sink_fn g_log_sink = NULL;
static log_level* g_util_level_ref = NULL;
static log_level* g_raop_level_ref = NULL;

void cross_set_logger(log_sink_fn fn) {
	g_log_sink = fn;
}

void cross_set_levels(log_level util_level, log_level raop_level) {
	if (g_util_level_ref) *g_util_level_ref = util_level;
	if (g_raop_level_ref) *g_raop_level_ref = raop_level;
}

static const char* source_from_level(log_level* current) {
	extern log_level util_loglevel;
	extern log_level raop_loglevel;
	if (current == &raop_loglevel) return "raop";
	if (current == &util_loglevel) return "util";
	return "unknown";
}

// logging functions
const char *logtime(void) {
	static char buf[100];
#ifdef _WIN32
	SYSTEMTIME lt;
	GetLocalTime(&lt);
	sprintf(buf, "[%02d:%02d:%02d.%03d]", lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	strftime(buf, sizeof(buf), "[%T.", localtime(&tv.tv_sec));
	sprintf(buf+strlen(buf), "%03ld]", (long)tv.tv_usec/1000);
#endif
	return buf;
}

/*---------------------------------------------------------------------------*/
void logprint_ex(log_level *current_level, const char *func, int line, const char *fmt, ...) {
	va_list args;
	if (!current_level) return;

	if (g_log_sink) {
		va_list args_copy;
		va_start(args, fmt);
		va_copy(args_copy, args);
		int needed = vsnprintf(NULL, 0, fmt, args_copy);
		va_end(args_copy);
		if (needed > 0) {
			size_t len = (size_t) needed + 64;
			char *msg = (char*) malloc(len + 1);
			if (msg) {
				int written = snprintf(msg, len, "%s %s:%d ", logtime(), func, line);
				if (written < 0) written = 0;
				vsnprintf(msg + written, len - (size_t)written, fmt, args);
				g_log_sink(source_from_level(current_level), msg, *current_level);
				free(msg);
				va_end(args);
				return;
			}
		}
		va_end(args);
		// fall through to stderr if allocation failed
	}

	va_start(args, fmt);
	fprintf(stderr, "%s %s:%d ", logtime(), func, line);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	fflush(stderr);
}

/*---------------------------------------------------------------------------*/
void logdump(const char* data, size_t size) {
	size_t count = 0;
	while (size) {
		size_t col = size > 16 ? 16 : size;
		fprintf(stderr, "%04x  ", (unsigned int) count);
		for (size_t i = 0; i < col; i++) fprintf(stderr, "%02x ", (unsigned char) data[i]);
		for (size_t i = 0; i < col; i++) isprint((unsigned char) data[i]) ? fputc(data[i], stderr) : fputc(' ', stderr);
		fputc('\n', stderr);
		data += col; size -= col; count += col;
	}
}

/*---------------------------------------------------------------------------*/
log_level debug2level(char *level)
{
	if (!strcmp(level, "error")) return lERROR;
	if (!strcmp(level, "warn")) return lWARN;
	if (!strcmp(level, "info")) return lINFO;
	if (!strcmp(level, "debug")) return lDEBUG;
	if (!strcmp(level, "sdebug")) return lSDEBUG;
	return lWARN;
}

/*---------------------------------------------------------------------------*/
char *level2debug(log_level level)
{
	switch (level) {
	case lERROR: return "error";
	case lWARN: return "warn";
	case lINFO: return "info";
	case lDEBUG: return "debug";
	case lSDEBUG: return "debug";
	default: return "warn";
	}
}
''')
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
patch_cross_log
bash "$ROOT_DIR/scripts/prune-vendor.sh"

touch "$LIB_DIR/.prepared"
echo "libraop sources already vendored under $LIB_DIR"
