/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */



#if ZLIB_VERNUM <= 0x1240
ZEXTERN int ZEXPORT gzclose_r _Z_OF((gzFile file));
ZEXTERN int ZEXPORT gzclose_w _Z_OF((gzFile file));
ZEXTERN int ZEXPORT gzbuffer _Z_OF((gzFile file, unsigned size));
ZEXTERN z_off_t ZEXPORT gzoffset _Z_OF((gzFile file));

#if !defined(_WIN32) && defined(Z_LARGE64)
#  define z_off64_t off64_t
#else
#  if defined(_WIN32) && !defined(__GNUC__) && !defined(Z_SOLO)
#    define z_off64_t __int64
#  else
#    define z_off64_t z_off_t
#  endif
#endif
#endif


#if ZLIB_VERNUM <= 0x1250
struct gzFile_s {
    unsigned have;
    unsigned char *next;
    z_off64_t pos;
};
#endif


#if ZLIB_VERNUM <= 0x1270
#if defined(_WIN32) && !defined(Z_SOLO)
#    include <stddef.h>         /* for wchar_t */
ZEXTERN gzFile         ZEXPORT gzopen_w _Z_OF((const wchar_t *path,
                                            const char *mode));
#endif
#endif


#if ZLIB_VERNUM < 0x12B0
#ifdef Z_SOLO
   typedef unsigned long z_size_t;
#else
#  define z_longlong long long
#  if defined(NO_SIZE_T)
     typedef unsigned NO_SIZE_T z_size_t;
#  elif defined(STDC)
#    include <stddef.h>
     typedef size_t z_size_t;
#  else
     typedef unsigned long z_size_t;
#  endif
#  undef z_longlong
#endif
ZEXTERN z_size_t ZEXPORT gzfread _Z_OF((voidp buf, z_size_t size, z_size_t nitems,
                                     gzFile file));
ZEXTERN z_size_t ZEXPORT gzfwrite _Z_OF((voidpc buf, z_size_t size,
                                      z_size_t nitems, gzFile file));
#endif
