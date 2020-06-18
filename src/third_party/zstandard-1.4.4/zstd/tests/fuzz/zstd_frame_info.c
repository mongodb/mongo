/*
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

/**
 * This fuzz target fuzzes all of the helper functions that consume compressed
 * input.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include "fuzz_helpers.h"
#include "zstd_helpers.h"

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    ZSTD_frameHeader zfh;
    /* You can fuzz any helper functions here that are fast, and take zstd
     * compressed data as input. E.g. don't expect the input to be a dictionary,
     * so don't fuzz ZSTD_getDictID_fromDict().
     */
    ZSTD_getFrameContentSize(src, size);
    ZSTD_getDecompressedSize(src, size);
    ZSTD_findFrameCompressedSize(src, size);
    ZSTD_getDictID_fromFrame(src, size);
    ZSTD_findDecompressedSize(src, size);
    ZSTD_decompressBound(src, size);
    ZSTD_frameHeaderSize(src, size);
    ZSTD_isFrame(src, size);
    ZSTD_getFrameHeader(&zfh, src, size);
    ZSTD_getFrameHeader_advanced(&zfh, src, size, ZSTD_f_zstd1);
    return 0;
}
