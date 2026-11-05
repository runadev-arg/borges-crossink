// Forced-include configuration for the cre-oracle host build. Mirrors the DOM-relevant
// settings of koreader-base thirdparty/kpvcrlib/crsetup.h.cmake; only image decoders,
// shaping and non-EPUB formats are off, none of which change the DOM or its xpointers.
#ifndef CRSETUP_H_INCLUDED
#define CRSETUP_H_INCLUDED
// KOReader compiles crengine as LINUX only ("Windows is unsupported"); its _WIN32 branches
// are dead code. So take every system header while mingw still sees _WIN32, then present the
// POSIX branch to crengine.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <locale.h>
#include <math.h>
#include <process.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#endif
#include <sys/mman.h>
#undef _WIN32
#undef _WIN64
#undef WIN32
#undef __WIN32__
#undef __WIN64__
#undef WIN64
#undef __MINGW32__
#undef __MINGW64__
#define LINUX 1
#ifndef CRE_ORACLE_NO_POSIX_SIGNALS
#define _LINUX 1
#endif
#define mkdir(path, mode) mkdir(path)
#define fsync(fd) 0
#define O_SYNC 0
static inline int posix_memalign(void** out, size_t, size_t size) { *out = malloc(size); return *out ? 0 : 12; }
#define USE_ZLIB 1
#define USE_ZSTD 0
#define DOCUMENT_CACHING_MIN_SIZE 0x10000
#define DOCUMENT_CACHING_SIZE_THRESHOLD 0x100000
#define CHM_SUPPORT_ENABLED 0
#define ENABLE_ANTIWORD 0
#define USE_MD4C 0
#define CR_EMULATE_GETTEXT 1
#define ARBITRARY_IMAGE_SCALE_ENABLED 1
#define MAX_IMAGE_SCALE_MUL 2
#define USE_GIF 0
#define USE_LIBJPEG 0
#define USE_LIBPNG 0
#define USE_LIBWEBP 0
#define USE_LUNASVG 0
#define USE_NANOSVG 0
#define MATHML_SUPPORT 1
#define USE_SRELL_REGEX 0
#define COLOR_BACKBUFFER 1
#define CR_INTERNAL_PAGE_ORIENTATION 1
#define GRAY_BACKBUFFER_BITS 2
#define GRAY_INVERSE 0
#define LVLONG_FILE_SUPPORT 0
#define USE_ANSI_FILES 1
#define FILE_STREAM_BUFFER_SIZE 0x20000
#define ZIP_STREAM_BUFFER_SIZE 0x40000
#define CR_USE_THREADS 0
#define LDOM_USE_OWN_MEM_MAN 1
#define USE_LIMITED_FONT_SIZES_SET 0
#define USE_BITMAP_FONTS 0
#define USE_WIN32_FONTS 0
#define USE_GLYPHCACHE_HASHTABLE 0
#define GLYPH_CACHE_SIZE 0x40000
#define ALLOW_KERNING 1
#define USE_FONTCONFIG 0
#define USE_FREETYPE 1
#define USE_FRIBIDI 0
#define USE_HARFBUZZ 0
#define USE_LIBUNIBREAK 0
#define USE_UTF8PROC 0
#define CR_ENABLE_PAGE_IMAGE_CACHE 0
#endif
