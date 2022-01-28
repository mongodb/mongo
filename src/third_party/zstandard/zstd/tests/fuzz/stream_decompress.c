/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/**
 * This fuzz target attempts to decompress the fuzzed data with the simple
 * decompression function to ensure the decompressor never crashes.
 */

#define ZSTD_STATIC_LINKING_ONLY

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include "fuzz_helpers.h"
#include "zstd.h"
#include "fuzz_data_producer.h"

static ZSTD_DStream *dstream = NULL;
uint32_t seed;

static ZSTD_outBuffer makeOutBuffer(FUZZ_dataProducer_t *producer, void* buf, size_t bufSize)
{
  ZSTD_outBuffer buffer = { buf, 0, 0 };

  if (FUZZ_dataProducer_empty(producer)) {
    buffer.size = bufSize;
  } else {
    buffer.size = (FUZZ_dataProducer_uint32Range(producer, 0, bufSize));
  }
  FUZZ_ASSERT(buffer.size <= bufSize);

  if (buffer.size == 0) {
    buffer.dst = NULL;
  }

  return buffer;
}

static ZSTD_inBuffer makeInBuffer(const uint8_t **src, size_t *size,
                                  FUZZ_dataProducer_t *producer)
{
  ZSTD_inBuffer buffer = { *src, 0, 0 };

  FUZZ_ASSERT(*size > 0);
  if (FUZZ_dataProducer_empty(producer)) {
    buffer.size = *size;
  } else {
    buffer.size = (FUZZ_dataProducer_uint32Range(producer, 0, *size));
  }
  FUZZ_ASSERT(buffer.size <= *size);
  *src += buffer.size;
  *size -= buffer.size;

  if (buffer.size == 0) {
    buffer.src = NULL;
  }

  return buffer;
}

int LLVMFuzzerTestOneInput(const uint8_t *src, size_t size)
{
    /* Give a random portion of src data to the producer, to use for
    parameter generation. The rest will be used for (de)compression */
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(src, size);
    int stableOutBuffer;
    ZSTD_outBuffer out;
    void* buf;
    size_t bufSize;
    size = FUZZ_dataProducer_reserveDataPrefix(producer);
    bufSize = MAX(10 * size, ZSTD_BLOCKSIZE_MAX);

    /* Allocate all buffers and contexts if not already allocated */
    buf = FUZZ_malloc(bufSize);

    if (!dstream) {
        dstream = ZSTD_createDStream();
        FUZZ_ASSERT(dstream);
    } else {
        FUZZ_ZASSERT(ZSTD_DCtx_reset(dstream, ZSTD_reset_session_only));
    }

    stableOutBuffer = FUZZ_dataProducer_uint32Range(producer, 0, 10) == 5;
    if (stableOutBuffer) {
      FUZZ_ZASSERT(ZSTD_DCtx_setParameter(dstream, ZSTD_d_stableOutBuffer, 1));
      out.dst = buf;
      out.size = bufSize;
      out.pos = 0;
    } else {
      out = makeOutBuffer(producer, buf, bufSize);
    }

    while (size > 0) {
        ZSTD_inBuffer in = makeInBuffer(&src, &size, producer);
        while (in.pos != in.size) {
            if (out.pos == out.size) {
                if (stableOutBuffer) goto error;
                out = makeOutBuffer(producer, buf, bufSize);
            }
            size_t const rc = ZSTD_decompressStream(dstream, &out, &in);
            if (ZSTD_isError(rc)) goto error;
        }
    }

error:
#ifndef STATEFUL_FUZZING
    ZSTD_freeDStream(dstream); dstream = NULL;
#endif
    FUZZ_dataProducer_free(producer);
    free(buf);
    return 0;
}
