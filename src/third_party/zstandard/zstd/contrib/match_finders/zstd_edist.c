/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/*-*************************************
*  Dependencies
***************************************/

/* Currently relies on qsort when combining contiguous matches. This can probabily 
 * be avoided but would require changes to the algorithm. The qsort is far from 
 * the bottleneck in this algorithm even for medium sized files so it's probably 
 * not worth trying to address */ 
#include <stdlib.h>
#include <assert.h>

#include "zstd_edist.h"
#include "mem.h"

/*-*************************************
*  Constants
***************************************/

/* Just a sential for the entires of the diagnomal matrix */
#define ZSTD_EDIST_DIAG_MAX (S32)(1 << 30)

/* How large should a snake be to be considered a 'big' snake. 
 * For an explanation of what a 'snake' is with respect to the 
 * edit distance matrix, see the linked paper in zstd_edist.h */
#define ZSTD_EDIST_SNAKE_THRESH 20

/* After how many iterations should we start to use the heuristic
 * based on 'big' snakes */
#define ZSTD_EDIST_SNAKE_ITER_THRESH 200

/* After how many iterations should be just give up and take 
 * the best availabe edit script for this round */ 
#define ZSTD_EDIST_EXPENSIVE_THRESH 1024

/*-*************************************
*  Structures
***************************************/

typedef struct {
    U32 dictIdx;
    U32 srcIdx;
    U32 matchLength;
} ZSTD_eDist_match;

typedef struct {
    const BYTE* dict;
    const BYTE* src;
    size_t dictSize;
    size_t srcSize;
    S32* forwardDiag;            /* Entires of the forward diagonal stored here */ 
    S32* backwardDiag;           /* Entires of the backward diagonal stored here.
                                  *   Note: this buffer and the 'forwardDiag' buffer 
                                  *   are contiguous. See the ZSTD_eDist_genSequences */ 
    ZSTD_eDist_match* matches;   /* Accumulate matches of length 1 in this buffer. 
                                  *   In a subsequence post-processing step, we combine 
                                  *   contiguous matches. */
    U32 nbMatches;
} ZSTD_eDist_state;

typedef struct {
    S32 dictMid;           /* The mid diagonal for the dictionary */
    S32 srcMid;            /* The mid diagonal for the source */ 
    int lowUseHeuristics;  /* Should we use heuristics for the low part */
    int highUseHeuristics; /* Should we use heuristics for the high part */ 
} ZSTD_eDist_partition;

/*-*************************************
*  Internal
***************************************/

static void ZSTD_eDist_diag(ZSTD_eDist_state* state,
                    ZSTD_eDist_partition* partition,
                    S32 dictLow, S32 dictHigh, S32 srcLow, 
                    S32 srcHigh, int useHeuristics)
{
    S32* const forwardDiag = state->forwardDiag;
    S32* const backwardDiag = state->backwardDiag;
    const BYTE* const dict = state->dict;
    const BYTE* const src = state->src;

    S32 const diagMin = dictLow - srcHigh;
    S32 const diagMax = dictHigh - srcLow;
    S32 const forwardMid = dictLow - srcLow;
    S32 const backwardMid = dictHigh - srcHigh;

    S32 forwardMin = forwardMid;
    S32 forwardMax = forwardMid;
    S32 backwardMin = backwardMid;
    S32 backwardMax = backwardMid;
    int odd = (forwardMid - backwardMid) & 1;
    U32 iterations;

    forwardDiag[forwardMid] = dictLow;
    backwardDiag[backwardMid] = dictHigh;

    /* Main loop for updating diag entries. Unless useHeuristics is 
     * set to false, this loop will run until it finds the minimal 
     * edit script */ 
    for (iterations = 1;;iterations++) {
        S32 diag;
        int bigSnake = 0;
        
        if (forwardMin > diagMin) {
            forwardMin--;
            forwardDiag[forwardMin - 1] = -1;
        } else {
            forwardMin++;
        }

        if (forwardMax < diagMax) {
            forwardMax++;
            forwardDiag[forwardMax + 1] = -1;
        } else {
            forwardMax--;
        }

        for (diag = forwardMax; diag >= forwardMin; diag -= 2) {
            S32 dictIdx;
            S32 srcIdx;
            S32 low = forwardDiag[diag - 1];
            S32 high = forwardDiag[diag + 1];
            S32 dictIdx0 = low < high ? high : low + 1;

            for (dictIdx = dictIdx0, srcIdx = dictIdx0 - diag;
                dictIdx < dictHigh && srcIdx < srcHigh && dict[dictIdx] == src[srcIdx];
                dictIdx++, srcIdx++) continue;

            if (dictIdx - dictIdx0 > ZSTD_EDIST_SNAKE_THRESH)
                bigSnake = 1;

            forwardDiag[diag] = dictIdx;

            if (odd && backwardMin <= diag && diag <= backwardMax && backwardDiag[diag] <= dictIdx) {
                partition->dictMid = dictIdx;
                partition->srcMid = srcIdx;
                partition->lowUseHeuristics = 0;
                partition->highUseHeuristics = 0;
                return;
            }
        }

        if (backwardMin > diagMin) {
            backwardMin--;
            backwardDiag[backwardMin - 1] = ZSTD_EDIST_DIAG_MAX;
        } else {
            backwardMin++;
        }

        if (backwardMax < diagMax) {
            backwardMax++;
            backwardDiag[backwardMax + 1] = ZSTD_EDIST_DIAG_MAX;
        } else {
            backwardMax--;
        }


        for (diag = backwardMax; diag >= backwardMin; diag -= 2) {
            S32 dictIdx;
            S32 srcIdx;
            S32 low = backwardDiag[diag - 1];
            S32 high = backwardDiag[diag + 1];
            S32 dictIdx0 = low < high ? low : high - 1;

            for (dictIdx = dictIdx0, srcIdx = dictIdx0 - diag;
                dictLow < dictIdx && srcLow < srcIdx && dict[dictIdx - 1] == src[srcIdx - 1];
                dictIdx--, srcIdx--) continue;

            if (dictIdx0 - dictIdx > ZSTD_EDIST_SNAKE_THRESH)
                bigSnake = 1;

            backwardDiag[diag] = dictIdx;

            if (!odd && forwardMin <= diag && diag <= forwardMax && dictIdx <= forwardDiag[diag]) {
                partition->dictMid = dictIdx;
                partition->srcMid = srcIdx;
                partition->lowUseHeuristics = 0;
                partition->highUseHeuristics = 0;
                return;
            }
        }

        if (!useHeuristics)
            continue;

        /* Everything under this point is a heuritic. Using these will 
         * substantially speed up the match finding. In some cases, taking 
         * the total match finding time from several minutes to seconds.
         * Of course, the caveat is that the edit script found may no longer 
         * be optimal */ 

        /* Big snake heuristic */ 
        if (iterations > ZSTD_EDIST_SNAKE_ITER_THRESH && bigSnake) {
            {
                S32 best = 0;
                
                for (diag = forwardMax; diag >= forwardMin; diag -= 2) {
                    S32 diagDiag = diag - forwardMid;
                    S32 dictIdx = forwardDiag[diag];
                    S32 srcIdx = dictIdx - diag;
                    S32 v = (dictIdx - dictLow) * 2 - diagDiag;

                    if (v > 12 * (iterations + (diagDiag < 0 ? -diagDiag : diagDiag))) {
                        if (v > best 
                          && dictLow + ZSTD_EDIST_SNAKE_THRESH <= dictIdx && dictIdx <= dictHigh
                          && srcLow + ZSTD_EDIST_SNAKE_THRESH <= srcIdx && srcIdx <= srcHigh) {
                            S32 k;
                            for (k = 1; dict[dictIdx - k] == src[srcIdx - k]; k++) {
                                if (k == ZSTD_EDIST_SNAKE_THRESH) {
                                    best = v;
                                    partition->dictMid = dictIdx;
                                    partition->srcMid = srcIdx;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (best > 0) {
                    partition->lowUseHeuristics = 0;
                    partition->highUseHeuristics = 1;
                    return;
                }
            }

            {
                S32 best = 0;

                for (diag = backwardMax; diag >= backwardMin; diag -= 2) {
                    S32 diagDiag = diag - backwardMid;
                    S32 dictIdx = backwardDiag[diag];
                    S32 srcIdx = dictIdx - diag;
                    S32 v = (dictHigh - dictIdx) * 2 + diagDiag;

                    if (v > 12 * (iterations + (diagDiag < 0 ? -diagDiag : diagDiag))) {
                        if (v > best 
                          && dictLow < dictIdx && dictIdx <= dictHigh - ZSTD_EDIST_SNAKE_THRESH
                          && srcLow < srcIdx && srcIdx <= srcHigh - ZSTD_EDIST_SNAKE_THRESH) {
                            int k;
                            for (k = 0; dict[dictIdx + k] == src[srcIdx + k]; k++) {
                                if (k == ZSTD_EDIST_SNAKE_THRESH - 1) { 
                                    best = v;
                                    partition->dictMid = dictIdx;
                                    partition->srcMid = srcIdx;
                                    break; 
                                }
                            }
                        }
                    }
                }

                if (best > 0) {
                    partition->lowUseHeuristics = 1;
                    partition->highUseHeuristics = 0;
                    return;
                }
            }
        }

        /* More general 'too expensive' heuristic */ 
        if (iterations >= ZSTD_EDIST_EXPENSIVE_THRESH) {
            S32 forwardDictSrcBest;
            S32 forwardDictBest = 0;
            S32 backwardDictSrcBest;
            S32 backwardDictBest = 0;

            forwardDictSrcBest = -1;
            for (diag = forwardMax; diag >= forwardMin; diag -= 2) {
                S32 dictIdx = MIN(forwardDiag[diag], dictHigh);
                S32 srcIdx = dictIdx - diag;

                if (srcHigh < srcIdx) {
                    dictIdx = srcHigh + diag;
                    srcIdx = srcHigh;
                }

                if (forwardDictSrcBest < dictIdx + srcIdx) {
                    forwardDictSrcBest = dictIdx + srcIdx;
                    forwardDictBest = dictIdx;
                }
            }

            backwardDictSrcBest = ZSTD_EDIST_DIAG_MAX;
            for (diag = backwardMax; diag >= backwardMin; diag -= 2) {
                S32 dictIdx = MAX(dictLow, backwardDiag[diag]);
                S32 srcIdx = dictIdx - diag;

                if (srcIdx < srcLow) {
                    dictIdx = srcLow + diag;
                    srcIdx = srcLow;
                }

                if (dictIdx + srcIdx < backwardDictSrcBest) {
                    backwardDictSrcBest = dictIdx + srcIdx;
                    backwardDictBest = dictIdx;
                }
            }

            if ((dictHigh + srcHigh) - backwardDictSrcBest < forwardDictSrcBest - (dictLow + srcLow)) {
                partition->dictMid = forwardDictBest;
                partition->srcMid = forwardDictSrcBest - forwardDictBest;
                partition->lowUseHeuristics = 0;
                partition->highUseHeuristics = 1;
            } else {
                partition->dictMid = backwardDictBest;
                partition->srcMid = backwardDictSrcBest - backwardDictBest;
                partition->lowUseHeuristics = 1;
                partition->highUseHeuristics = 0;
            }
            return;
        }
    }
}

static void ZSTD_eDist_insertMatch(ZSTD_eDist_state* state, 
                    S32 const dictIdx, S32 const srcIdx)
{
    state->matches[state->nbMatches].dictIdx = dictIdx;
    state->matches[state->nbMatches].srcIdx = srcIdx;
    state->matches[state->nbMatches].matchLength = 1;
    state->nbMatches++;
}

static int ZSTD_eDist_compare(ZSTD_eDist_state* state,
                    S32 dictLow, S32 dictHigh, S32 srcLow,
                    S32 srcHigh, int useHeuristics)
{
    const BYTE* const dict = state->dict;
    const BYTE* const src = state->src;

    /* Found matches while traversing from the low end */ 
    while (dictLow < dictHigh && srcLow < srcHigh && dict[dictLow] == src[srcLow]) {
        ZSTD_eDist_insertMatch(state, dictLow, srcLow);
        dictLow++;
        srcLow++;
    }

    /* Found matches while traversing from the high end */
    while (dictLow < dictHigh && srcLow < srcHigh && dict[dictHigh - 1] == src[srcHigh - 1]) {
        ZSTD_eDist_insertMatch(state, dictHigh - 1, srcHigh - 1);
        dictHigh--;
        srcHigh--;
    }
    
    /* If the low and high end end up touching. If we wanted to make 
     * note of the differences like most diffing algorithms do, we would 
     * do so here. In our case, we're only concerned with matches 
     * Note: if you wanted to find the edit distance of the algorithm, 
     *   you could just accumulate the cost for an insertion/deletion 
     *   below. */ 
    if (dictLow == dictHigh) {
        while (srcLow < srcHigh) {
            /* Reaching this point means inserting src[srcLow] into 
             * the current position of dict */ 
            srcLow++;
        }
    } else if (srcLow == srcHigh) {
        while (dictLow < dictHigh) {
            /* Reaching this point means deleteing dict[dictLow] from 
             * the current positino of dict */ 
            dictLow++;
        }
    } else {
        ZSTD_eDist_partition partition;
        partition.dictMid = 0;
        partition.srcMid = 0;
        ZSTD_eDist_diag(state, &partition, dictLow, dictHigh, 
            srcLow, srcHigh, useHeuristics);
        if (ZSTD_eDist_compare(state, dictLow, partition.dictMid, 
          srcLow, partition.srcMid, partition.lowUseHeuristics))
            return 1;
        if (ZSTD_eDist_compare(state, partition.dictMid, dictHigh,
          partition.srcMid, srcHigh, partition.highUseHeuristics))
            return 1;
    }

    return 0;
}

static int ZSTD_eDist_matchComp(const void* p, const void* q)
{
    S32 const l = ((ZSTD_eDist_match*)p)->srcIdx;
    S32 const r = ((ZSTD_eDist_match*)q)->srcIdx;
    return (l - r);
}

/* The matches from the approach above will all be of the form 
 * (dictIdx, srcIdx, 1). this method combines contiguous matches 
 * of length MINMATCH or greater. Matches less than MINMATCH 
 * are discarded */ 
static void ZSTD_eDist_combineMatches(ZSTD_eDist_state* state)
{
    /* Create a new buffer to put the combined matches into 
     * and memcpy to state->matches after */ 
    ZSTD_eDist_match* combinedMatches = 
        ZSTD_malloc(state->nbMatches * sizeof(ZSTD_eDist_match), 
        ZSTD_defaultCMem);

    U32 nbCombinedMatches = 1;
    size_t i;

    /* Make sure that the srcIdx and dictIdx are in sorted order.
     * The combination step won't work otherwise */ 
    qsort(state->matches, state->nbMatches, sizeof(ZSTD_eDist_match), ZSTD_eDist_matchComp);

    memcpy(combinedMatches, state->matches, sizeof(ZSTD_eDist_match));
    for (i = 1; i < state->nbMatches; i++) {
        ZSTD_eDist_match const match = state->matches[i];
        ZSTD_eDist_match const combinedMatch = 
            combinedMatches[nbCombinedMatches - 1];
        if (combinedMatch.srcIdx + combinedMatch.matchLength == match.srcIdx && 
          combinedMatch.dictIdx + combinedMatch.matchLength == match.dictIdx) {
            combinedMatches[nbCombinedMatches - 1].matchLength++;
        } else {
            /* Discard matches that are less than MINMATCH */
            if (combinedMatches[nbCombinedMatches - 1].matchLength < MINMATCH) {
                nbCombinedMatches--;
            }

            memcpy(combinedMatches + nbCombinedMatches, 
                state->matches + i, sizeof(ZSTD_eDist_match));
            nbCombinedMatches++;
        }
    }
    memcpy(state->matches, combinedMatches, nbCombinedMatches * sizeof(ZSTD_eDist_match));
    state->nbMatches = nbCombinedMatches;
    ZSTD_free(combinedMatches, ZSTD_defaultCMem);
}

static size_t ZSTD_eDist_convertMatchesToSequences(ZSTD_Sequence* sequences, 
    ZSTD_eDist_state* state)
{
    const ZSTD_eDist_match* matches = state->matches;
    size_t const nbMatches = state->nbMatches;
    size_t const dictSize = state->dictSize;
    size_t nbSequences = 0;
    size_t i;
    for (i = 0; i < nbMatches; i++) {
        ZSTD_eDist_match const match = matches[i];
        U32 const litLength = !i ? match.srcIdx : 
            match.srcIdx - (matches[i - 1].srcIdx + matches[i - 1].matchLength);
        U32 const offset = (match.srcIdx + dictSize) - match.dictIdx;
        U32 const matchLength = match.matchLength;
        sequences[nbSequences].offset = offset;
        sequences[nbSequences].litLength = litLength;
        sequences[nbSequences].matchLength = matchLength;
        nbSequences++;
    }
    return nbSequences;
}

/*-*************************************
*  Interal utils
***************************************/

static size_t ZSTD_eDist_hamingDist(const BYTE* const a,
                        const BYTE* const b, size_t n)
{
    size_t i;
    size_t dist = 0;
    for (i = 0; i < n; i++)
        dist += a[i] != b[i];
    return dist; 
}

/* This is a pretty naive recursive implementation that should only
 * be used for quick tests obviously. Don't try and run this on a 
 * GB file or something. There are faster implementations. Use those
 * if you need to run it for large files. */
static size_t ZSTD_eDist_levenshteinDist(const BYTE* const s,
                        size_t const sn, const BYTE* const t,
                        size_t const tn)
{
    size_t a, b, c;

    if (!sn)
        return tn;
    if (!tn)
        return sn;
    
    if (s[sn - 1] == t[tn - 1])
        return ZSTD_eDist_levenshteinDist(
            s, sn - 1, t, tn - 1);
    
    a = ZSTD_eDist_levenshteinDist(s, sn - 1, t, tn - 1);
    b = ZSTD_eDist_levenshteinDist(s, sn, t, tn - 1);
    c = ZSTD_eDist_levenshteinDist(s, sn - 1, t, tn);

    if (a > b)
        a = b;
    if (a > c)
        a = c;
    
    return a + 1;
}

static void ZSTD_eDist_validateMatches(ZSTD_eDist_match* matches,
                        size_t const nbMatches, const BYTE* const dict,
                        size_t const dictSize, const BYTE* const src,
                        size_t const srcSize)
{
    size_t i;
    for (i = 0; i < nbMatches; i++) {
        ZSTD_eDist_match match = matches[i];
        U32 const dictIdx = match.dictIdx;
        U32 const srcIdx = match.srcIdx;
        U32 const matchLength = match.matchLength;
        
        assert(dictIdx + matchLength < dictSize);
        assert(srcIdx + matchLength < srcSize);
        assert(!memcmp(dict + dictIdx, src + srcIdx, matchLength));
    }
}

/*-*************************************
*  API
***************************************/

size_t ZSTD_eDist_genSequences(ZSTD_Sequence* sequences, 
                        const void* dict, size_t dictSize,
                        const void* src, size_t srcSize,
                        int useHeuristics)
{
    size_t const nbDiags = dictSize + srcSize + 3;
    S32* buffer = ZSTD_malloc(nbDiags * 2 * sizeof(S32), ZSTD_defaultCMem);
    ZSTD_eDist_state state;
    size_t nbSequences = 0;

    state.dict = (const BYTE*)dict;
    state.src = (const BYTE*)src;
    state.dictSize = dictSize;
    state.srcSize = srcSize;
    state.forwardDiag = buffer;
    state.backwardDiag = buffer + nbDiags;
    state.forwardDiag += srcSize + 1;
    state.backwardDiag += srcSize + 1;
    state.matches = ZSTD_malloc(srcSize * sizeof(ZSTD_eDist_match), ZSTD_defaultCMem);
    state.nbMatches = 0;

    ZSTD_eDist_compare(&state, 0, dictSize, 0, srcSize, 1);
    ZSTD_eDist_combineMatches(&state);
    nbSequences = ZSTD_eDist_convertMatchesToSequences(sequences, &state);

    ZSTD_free(buffer, ZSTD_defaultCMem);
    ZSTD_free(state.matches, ZSTD_defaultCMem);

    return nbSequences;
}
