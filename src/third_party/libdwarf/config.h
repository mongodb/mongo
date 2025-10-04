/**
 * Based on https://github.com/davea42/libdwarf-code/blob/main/cmake/config.h.in.
 * This file is a manually created libdwarf config.h for MongoDB environments.
 */

#if __has_include(<fcntl.h>)
#define HAVE_FCNTL_H 1
#endif

#if __has_include(<sys/mman.h>)
#define HAVE_FULL_MMAP 1
#endif

#if __has_include(<stdint.h>)
#define HAVE_STDINT_H 1
#endif

#if __has_include(<sys/stat.h>)
#define HAVE_SYS_STAT_H 1
#endif

#if __has_include(<sys/types.h>)
#define HAVE_SYS_TYPES_H 1
#endif

#if __has_include(<unistd.h>)
#define HAVE_UNISTD_H 1
#endif

#define HAVE_UTF8 1

#define HAVE_ZLIB 1
#define HAVE_ZLIB_H 1
#define HAVE_ZSTD 1
#define HAVE_ZSTD_H 1

#define PACKAGE_VERSION "2.1.0"

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WORDS_BIGENDIAN 1
#endif
