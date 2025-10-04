/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "zstd.h"
#include "zstd_seekable.h"
#include "fuzz_helpers.h"
#include "fuzz_data_producer.h"

static ZSTD_seekable *stream = NULL;
static ZSTD_seekable_CStream *zscs = NULL;
static const size_t kSeekableOverheadSize = ZSTD_seekTableFooterSize;

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    size = FUZZ_dataProducer_reserveDataPrefix(producer);
    size_t const compressedBufferSize = ZSTD_compressBound(size) + kSeekableOverheadSize;
    uint8_t* compressedBuffer = (uint8_t*)malloc(compressedBufferSize);
    uint8_t* decompressedBuffer = (uint8_t*)malloc(size);

    int const cLevel = FUZZ_dataProducer_int32Range(producer, ZSTD_minCLevel(), ZSTD_maxCLevel());
    unsigned const checksumFlag = FUZZ_dataProducer_int32Range(producer, 0, 1);
    size_t const uncompressedSize = FUZZ_dataProducer_uint32Range(producer, 0, size);
    size_t const offset = FUZZ_dataProducer_uint32Range(producer, 0, size - uncompressedSize);
    size_t seekSize;

    if (!zscs) {
        zscs = ZSTD_seekable_createCStream();
        FUZZ_ASSERT(zscs);
    }
    if (!stream) {
        stream = ZSTD_seekable_create();
        FUZZ_ASSERT(stream);
    }

    {   /* Perform a compression */
        size_t const initStatus = ZSTD_seekable_initCStream(zscs, cLevel, checksumFlag, size);
        size_t endStatus;
        ZSTD_outBuffer out = { .dst=compressedBuffer, .pos=0, .size=compressedBufferSize };
        ZSTD_inBuffer  in  = { .src=src, .pos=0, .size=size };
        FUZZ_ASSERT(!ZSTD_isError(initStatus));

        do {
            size_t cSize = ZSTD_seekable_compressStream(zscs, &out, &in);
            FUZZ_ASSERT(!ZSTD_isError(cSize));
        } while (in.pos != in.size);

        FUZZ_ASSERT(in.pos == in.size);
        endStatus = ZSTD_seekable_endStream(zscs, &out);
        FUZZ_ASSERT(!ZSTD_isError(endStatus));
        seekSize = out.pos;
    }

    {   /* Decompress at an offset */
        size_t const initStatus = ZSTD_seekable_initBuff(stream, compressedBuffer, seekSize);
        size_t decompressedBytesTotal = 0;
        size_t dSize;

        FUZZ_ZASSERT(initStatus);
        do {
            dSize = ZSTD_seekable_decompress(stream, decompressedBuffer, uncompressedSize, offset);
            FUZZ_ASSERT(!ZSTD_isError(dSize));
            decompressedBytesTotal += dSize;
        } while (decompressedBytesTotal < uncompressedSize && dSize > 0);
        FUZZ_ASSERT(decompressedBytesTotal == uncompressedSize);
    }

    FUZZ_ASSERT_MSG(!FUZZ_memcmp(src+offset, decompressedBuffer, uncompressedSize), "Corruption!");

    free(decompressedBuffer);
    free(compressedBuffer);
    FUZZ_dataProducer_free(producer);

#ifndef STATEFUL_FUZZING
    ZSTD_seekable_free(stream); stream = NULL;
    ZSTD_seekable_freeCStream(zscs); zscs = NULL;
#endif
    return 0;
}
