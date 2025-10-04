/**
 * \file zstd.c
 * Single-file Zstandard library.
 *
 * Generate using:
 * \code
 *	python combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstd.c zstd-in.c
 * \endcode
 */
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
/*
 * Settings to bake for the single library file.
 *
 * Note: It's important that none of these affects 'zstd.h' (only the
 * implementation files we're amalgamating).
 *
 * Note: MEM_MODULE stops xxhash redefining BYTE, U16, etc., which are also
 * defined in mem.h (breaking C99 compatibility).
 *
 * Note: the undefs for xxHash allow Zstd's implementation to coincide with
 * standalone xxHash usage (with global defines).
 *
 * Note: if you enable ZSTD_LEGACY_SUPPORT the combine.py script will need
 * re-running without the "-x legacy/zstd_legacy.h" option (it excludes the
 * legacy support at the source level).
 *
 * Note: multithreading is enabled for all platforms apart from Emscripten.
 */
#define DEBUGLEVEL 0
#define MEM_MODULE
#undef  XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#undef  XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef  XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#ifndef __EMSCRIPTEN__
#define ZSTD_MULTITHREAD
#endif
#define ZSTD_TRACE 0
/* TODO: Can't amalgamate ASM function */
#define ZSTD_DISABLE_ASM 1

/* Include zstd_deps.h first with all the options we need enabled. */
#define ZSTD_DEPS_NEED_MALLOC
#define ZSTD_DEPS_NEED_MATH64
#include "common/zstd_deps.h"

#include "common/debug.c"
#include "common/entropy_common.c"
#include "common/error_private.c"
#include "common/fse_decompress.c"
#include "common/threading.c"
#include "common/pool.c"
#include "common/zstd_common.c"

#include "compress/fse_compress.c"
#include "compress/hist.c"
#include "compress/huf_compress.c"
#include "compress/zstd_compress_literals.c"
#include "compress/zstd_compress_sequences.c"
#include "compress/zstd_compress_superblock.c"
#include "compress/zstd_compress.c"
#include "compress/zstd_double_fast.c"
#include "compress/zstd_fast.c"
#include "compress/zstd_lazy.c"
#include "compress/zstd_ldm.c"
#include "compress/zstd_opt.c"
#ifdef ZSTD_MULTITHREAD
#include "compress/zstdmt_compress.c"
#endif

#include "decompress/huf_decompress.c"
#include "decompress/zstd_ddict.c"
#include "decompress/zstd_decompress.c"
#include "decompress/zstd_decompress_block.c"

#include "dictBuilder/cover.c"
#include "dictBuilder/divsufsort.c"
#include "dictBuilder/fastcover.c"
#include "dictBuilder/zdict.c"
