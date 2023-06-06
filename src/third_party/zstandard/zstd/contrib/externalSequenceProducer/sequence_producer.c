/*
 * Copyright (c) Yann Collet, Meta Platforms, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "zstd_compress_internal.h"
#include "sequence_producer.h"

#define HSIZE 1024
static U32 const HLOG = 10;
static U32 const MLS = 4;
static U32 const BADIDX = 0xffffffff;

size_t simpleSequenceProducer(
  void* sequenceProducerState,
  ZSTD_Sequence* outSeqs, size_t outSeqsCapacity,
  const void* src, size_t srcSize,
  const void* dict, size_t dictSize,
  int compressionLevel,
  size_t windowSize
) {
    const BYTE* const istart = (const BYTE*)src;
    const BYTE* const iend = istart + srcSize;
    const BYTE* ip = istart;
    const BYTE* anchor = istart;
    size_t seqCount = 0;
    U32 hashTable[HSIZE];

    (void)sequenceProducerState;
    (void)dict;
    (void)dictSize;
    (void)outSeqsCapacity;
    (void)compressionLevel;

    {   int i;
        for (i=0; i < HSIZE; i++) {
            hashTable[i] = BADIDX;
    }   }

    while (ip + MLS < iend) {
        size_t const hash = ZSTD_hashPtr(ip, HLOG, MLS);
        U32 const matchIndex = hashTable[hash];
        hashTable[hash] = (U32)(ip - istart);

        if (matchIndex != BADIDX) {
            const BYTE* const match = istart + matchIndex;
            U32 const matchLen = (U32)ZSTD_count(ip, match, iend);
            if (matchLen >= ZSTD_MINMATCH_MIN) {
                U32 const litLen = (U32)(ip - anchor);
                U32 const offset = (U32)(ip - match);
                ZSTD_Sequence const seq = {
                    offset, litLen, matchLen, 0
                };

                /* Note: it's crucial to stay within the window size! */
                if (offset <= windowSize) {
                    outSeqs[seqCount++] = seq;
                    ip += matchLen;
                    anchor = ip;
                    continue;
                }
            }
        }

        ip++;
    }

    {   ZSTD_Sequence const finalSeq = {
            0, (U32)(iend - anchor), 0, 0
        };
        outSeqs[seqCount++] = finalSeq;
    }

    return seqCount;
}
