/*
 * Copyright (c) Yann Collet, Meta Platforms, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef EXTERNAL_MATCHFINDER
#define EXTERNAL_MATCHFINDER

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

/* See external_matchfinder.c for details on each test case */
typedef enum {
    EMF_ZERO_SEQS = 0,
    EMF_ONE_BIG_SEQ = 1,
    EMF_LOTS_OF_SEQS = 2,
    EMF_BIG_ERROR = 3,
    EMF_SMALL_ERROR = 4,
    EMF_INVALID_OFFSET = 5,
    EMF_INVALID_MATCHLEN = 6,
    EMF_INVALID_LITLEN = 7,
    EMF_INVALID_LAST_LITS = 8
} EMF_testCase;

size_t zstreamSequenceProducer(
  void* sequenceProducerState,
  ZSTD_Sequence* outSeqs, size_t outSeqsCapacity,
  const void* src, size_t srcSize,
  const void* dict, size_t dictSize,
  int compressionLevel,
  size_t windowSize
);

#endif /* EXTERNAL_MATCHFINDER */
