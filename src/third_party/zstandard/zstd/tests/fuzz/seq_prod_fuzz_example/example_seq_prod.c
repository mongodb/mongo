/*
 * Copyright (c) Yann Collet, Meta Platforms, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "fuzz_third_party_seq_prod.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

_Thread_local size_t threadLocalState;

size_t FUZZ_seqProdSetup(void) {
    threadLocalState = 0;
    return 0;
}

size_t FUZZ_seqProdTearDown(void) {
    return 0;
}

void* FUZZ_createSeqProdState(void) {
    return calloc(1, sizeof(size_t));
}

size_t FUZZ_freeSeqProdState(void* state) {
    free(state);
    return 0;
}

size_t FUZZ_thirdPartySeqProd(
    void* sequenceProducerState,
    ZSTD_Sequence* outSeqs, size_t outSeqsCapacity,
    const void* src, size_t srcSize,
    const void* dict, size_t dictSize,
    int compressionLevel,
    size_t windowSize
) {
    /* Try to catch unsafe use of the shared state */
    size_t* const sharedStatePtr = (size_t*)sequenceProducerState;
    assert(*sharedStatePtr == threadLocalState);
    (*sharedStatePtr)++; threadLocalState++;

    /* Check that fallback is enabled when FUZZ_THIRD_PARTY_SEQ_PROD is defined */
    return ZSTD_SEQUENCE_PRODUCER_ERROR;
}
