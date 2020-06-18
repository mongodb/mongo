/*
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

/**
 * Helper functions for fuzzing.
 */

#ifndef ZSTD_HELPERS_H
#define ZSTD_HELPERS_H

#define ZSTD_STATIC_LINKING_ONLY

#include "zstd.h"
#include "fuzz_data_producer.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const int kMinClevel;
extern const int kMaxClevel;

void FUZZ_setRandomParameters(ZSTD_CCtx *cctx, size_t srcSize, FUZZ_dataProducer_t *producer);

ZSTD_compressionParameters FUZZ_randomCParams(size_t srcSize, FUZZ_dataProducer_t *producer);
ZSTD_frameParameters FUZZ_randomFParams(FUZZ_dataProducer_t *producer);
ZSTD_parameters FUZZ_randomParams(size_t srcSize, FUZZ_dataProducer_t *producer);

typedef struct {
  void* buff;
  size_t size;
} FUZZ_dict_t;

/* Quickly train a dictionary from a source for fuzzing.
 * NOTE: Don't use this to train production dictionaries, it is only optimized
 * for speed, and doesn't care about dictionary quality.
 */
FUZZ_dict_t FUZZ_train(void const* src, size_t srcSize, FUZZ_dataProducer_t *producer);

#ifdef __cplusplus
}
#endif

#endif /* ZSTD_HELPERS_H */
