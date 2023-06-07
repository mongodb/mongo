/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPLC_DEFLATE_DEFS_H_
#define QPLC_DEFLATE_DEFS_H_

#include "encode_df.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OWN_BYTES_FOR_HASH_CALCULATION 4u     /**> Number of bytes that is used for hash calculation*/
#define OWN_MINIMAL_MATCH_LENGTH 4u           /**> Minimal match length used during match search */
#define OWN_MAXIMAL_OFFSET 4096u              /**> Maximal offset for match */
#define OWN_UNINITIALIZED_INDEX 0xFFFFFFFFu   /**> Value of uninitialized index in hash table*/
#define LITERAL_DISTANCE_IN_ICF 30

/**
 * @brief Internal structure that contains information about found match
 */
typedef struct {

    /**
     * Length of the match
     */
    uint32_t length;

    /**
     * Index in the input text (is being calculated using lower boundary during the search)
     */
    uint32_t index;

    /**
     * An offset from the matched string
     */
    uint32_t offset;

    /**
     * Hash value of the match
     */
    uint32_t hash;

#ifdef SCORE_FUNCTION
    /**
     * The score that was calculated using DCG score function
     */
    uint32_t score;
#endif

    /**
     * Pointer to the match in the text
     */
    uint8_t *match_source_ptr;
} deflate_match_t;

typedef struct deflate_icf deflate_icf;

struct deflate_icf_stream {
    deflate_icf *begin_ptr;
    deflate_icf *next_ptr;
    deflate_icf *end_ptr;
};

#ifdef __cplusplus
}
#endif

#endif // QPLC_DEFLATE_DEFS_H_ 
