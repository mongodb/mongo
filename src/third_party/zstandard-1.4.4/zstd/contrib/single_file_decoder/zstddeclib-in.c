/**
 * \file zstddeclib.c
 * Single-file Zstandard decompressor.
 * 
 * Generate using:
 * \code
 *	combine.sh -r ../../lib -r ../../lib/common -r ../../lib/decompress -o zstddeclib.c zstddeclib-in.c
 * \endcode
 */
/* 
 * BSD License
 * 
 * For Zstandard software
 * 
 * Copyright (c) 2016-present, Facebook, Inc. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name Facebook nor the names of its contributors may be used
 *   to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Settings to bake for the standalone decompressor.
 * 
 * Note: It's important that none of these affects 'zstd.h' (only the
 * implementation files we're amalgamating).
 * 
 * Note: MEM_MODULE stops xxhash redefining BYTE, U16, etc., which are also
 * defined in mem.h (breaking C99 compatibility).
 */
#define DEBUGLEVEL 0
#define MEM_MODULE
#define XXH_NAMESPACE ZSTD_
#define XXH_PRIVATE_API
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_LIB_COMPRESSION 0
#define ZSTD_LIB_DEPRECATED 0
#define ZSTD_NOBENCH
#define ZSTD_STRIP_ERROR_STRINGS

#include "debug.c"
#include "entropy_common.c"
#include "error_private.c"
#include "fse_decompress.c"
#include "xxhash.c"
#include "zstd_common.c"
#include "huf_decompress.c"
#include "zstd_ddict.c"
#include "zstd_decompress.c"
#include "zstd_decompress_block.c"
