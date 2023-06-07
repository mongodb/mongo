/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @file owndeflatejob.h
 * @brief service functions for @link own_deflate_job @endlink internal structure
 */

#ifndef QPL_PROJECT_OWN_DEFLATE_JOB_H
#define QPL_PROJECT_OWN_DEFLATE_JOB_H

#include "qpl/c_api/statistics.h"
#include "stdbool.h"
#include "deflate_histogram.h"
#include "bit_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------ Internal types ------ */

/**
 * @brief Simple redefining for style purposes
 */
typedef struct isal_hufftables isal_huffman_table;

/**
 * @brief Common enumeration that is used for block types definitions
 */
typedef enum {
    initial_status = 0u,    /**< Informs that encoding is just starting */
    final_status   = 1u,    /**< Informs that encoding should be finished */
    running_status = 2u,    /**< Informs that encoding was already started but is not yet final*/
} own_current_status;

/**
 * @brief Internal structure that is used for deflating
 */
typedef struct {

    /**
     * Pointer to start of the input bytes that should be encoded
     */
    uint8_t *lower_bound_ptr;

    /**
     * Pointer to current byte that is being processed
     */
    uint8_t *current_ptr;

    /**
     * Pointer to end of the input bytes that should be encoded
     */
    uint8_t *upper_bound_ptr;

    /**
     * Pointer to @link own_deflate_histogram @endlink for statistics gathering
     */
    deflate_histogram_t *histogram_ptr;

    /**
     * Pointer to @link isal_huffman_table @endlink for Huffman encoding
     */
    isal_huffman_table *huffman_table_ptr;

    /**
     * Structure @link own_bit_writer @endlink for writing into output
     */
    bit_writer_t bit_writer;

    /**
     * Number of input bytes
     */
    uint32_t input_bytes;

    /**
     * Number of bytes that were read during deflate
     */
    uint32_t bytes_read;

    /**
     * Block type (initial, final, running)
     */
    own_current_status block_status;

    /**
     * Job status (initial, final, running)
     */
    own_current_status job_status;

    /**
     * Statistics mode for compression
     */
    qpl_statistics_mode statistics_mode;
} own_deflate_job;

/**
 * @brief Simple callback that can process information in @link own_deflate_job @endlink
 */
typedef void (*own_deflate_job_callback)(own_deflate_job *const job_ptr);

/**
 * @brief Simple predicate that can perform some simple conditions
 */
typedef uint8_t (*own_deflate_job_predicate)(own_deflate_job *const job_ptr);

/**
 * @brief Function type that can be used for matches searching
 *
 * @param[in]  hash_table_ptr   pointer to @link own_deflate_hash_table @endlink that is used for searching
 * @param[in]  lower_bound_ptr  pointer to lower boundary of the search
 * @param[in]  string_ptr       pointer to string that could be repeated previously in the text
 * @param[in]  upper_bound_ptr  pointer to upper boundary of the search
 *
 * @return @link own_match @endlink with information about discovered match
 */
typedef deflate_match_t (*own_deflate_match_finder)(const deflate_hash_table_t *hash_table_ptr,
                                                    const uint8_t *lower_bound_ptr,
                                                    const uint8_t *string_ptr,
                                                    const uint8_t *upper_bound_ptr);

/**
 * @brief Function type that can be used for literals processing
 *
 * @param[in,out]  job_ptr          pointer to @link own_deflate_job @endlink that will be processing literals
 * @param[in]      upper_bound_ptr  pointer to upper boundary of the processing
 * @param[in]      safe             flag if processing should be done safely or not
 */
typedef void (*own_deflate_literals_handler)(own_deflate_job *const job_ptr,
                                             const uint8_t *upper_bound_ptr,
                                             bool safe);

/**
 * @brief Function type that can be used for matches processing
 *
 * @param[in,out]  job_ptr           pointer to @link own_deflate_job @endlink that will be processing match
 * @param[in]      match             information about found match
 * @param[in]      literals_handler  function that will be used for processing missed literals
 */
typedef void (*own_deflate_matches_handler)(own_deflate_job *const job_ptr,
                                            const deflate_match_t match,
                                            const own_deflate_literals_handler literals_handler);

/* ------ Own functions API ------ */

/**
 * @brief Performs switching of current_ptr in @link own_deflate_job @endlink on giving range
 *
 * @param[in,out]  job_ptr  pointer to @link own_deflate_job @endlink
 * @param[in]      range    distance on which current_ptr should be moved
 */
void own_deflate_job_switch_to_next(own_deflate_job *const job_ptr, const uint32_t range);

/**
 * @brief Performs common deflate pipeline using given parts for specific tasks
 *
 * @param[in,out]  job_ptr           pointer to @link own_deflate_job @endlink that will contain all of
 *                                   the information
 * @param[in]      match_finder      @link own_deflate_match_finder @endlink that will be used for matches discovering
 * @param[in]      literals_handler  @link own_deflate_literals_handler @endlink that will be used for all literals
 *                                   processing
 * @param[in]      matches_handler   @link own_deflate_matches_handler @endlink that will be used for all matches
 *                                   processing
 * @param[in]      callback          @link own_deflate_job_callback @endlink that will be executed at
 *                                   the end of deflating
 */
void own_deflate_job_perform(own_deflate_job *const job_ptr,
                             const own_deflate_match_finder match_finder,
                             const own_deflate_literals_handler literals_handler,
                             const own_deflate_matches_handler matches_handler,
                             const own_deflate_job_predicate predicate,
                             const own_deflate_job_callback callback);

/**
 * @brief Performs statistics gathering in given boundaries for literals, but doesn't update instructions logic field
 *
 * @param[in,out]  job_ptr          pointer to @link own_deflate_job @endlink where this statistics should be stored
 * @param[in]      upper_bound_ptr  pointer to the upper boundary of the literals where processing should be ended
 * @param[in]      safe             flag if processing should be done safely or not
 *
 * @note API of this function should implement @link own_deflate_literals_handler @endlink
 */
void own_deflate_job_process_literals_no_instructions(own_deflate_job *const job_ptr,
                                                      const uint8_t *const upper_bound_ptr,
                                                      bool safe);

/**
 * @brief Performs statistics gathering for the found match could be actually discovered using "lazy" logic
 *        so it also has to know what to do with missed literals.
 * 
 * This functions does everything pretty much the same as @link own_deflate_job_process_match @endlink,
 * but doesn't update instructions logic field
 *
 * @param[in,out]  job_ptr           pointer to @link own_deflate_job @endlink where this statistics should be stored
 * @param[in]      match             @link own_match @endlink that contains information about the found match
 * @param[in]      literals_handler  handler that should be used for literals processing
 *
 * @note API of this function should implement @link own_deflate_matches_handler @endlink
 */
void own_deflate_job_process_match_no_instructions(own_deflate_job *const job_ptr,
                                                   const deflate_match_t match,
                                                   const own_deflate_literals_handler literals_handler);

/**
 * @brief Resets input source for deflate processing
 *
 * @param[out]  job_ptr      pointer to @link own_deflate_job @endlink where input should be set
 * @param[in]   source_ptr   pointer to bytes that should be encoded
 * @param[in]   input_bytes  number of input bytes
 *
 * @note Sets number of read bytes to zero
 */
void own_deflate_job_reset_input(own_deflate_job *const job_ptr,
                                 const uint8_t *const source_ptr,
                                 const uint32_t input_bytes);

/**
 * @brief Simple initializing function that acts as constructor for @link own_deflate_job @endlink
 *
 * @param[out]  job_ptr       pointer to @link own_deflate_job @endlink that should be initialized
 * @param[in]   source_ptr    pointer to bytes that should be encoded
 * @param[in]   input_bytes   number of input bytes
 * @param[in]   output_ptr    pointer to output array
 * @param[in]   output_bytes  number of available output bytes
 * @param[in]   block_status  @link own_current_status @endlink that informs what type of block is being encoded in this job
 */
void own_initialize_deflate_job(own_deflate_job *const job_ptr,
                                const uint8_t *const source_ptr,
                                const uint32_t input_bytes,
                                const uint8_t *const output_ptr,
                                const uint32_t output_bytes,
                                const own_current_status block_status,
                                const qpl_statistics_mode statistics_mode);

/**
 * @brief Updates histogram for given @ref own_deflate_job
 */
void own_update_deflate_histogram_high_level (own_deflate_job *deflate_job_ptr);

#ifdef __cplusplus
}
#endif

#endif // QPL_PROJECT_OWN_DEFLATE_JOB_H
