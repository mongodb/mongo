/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C++ API)
 */

#ifndef QPL_JOB_API_H_
#define QPL_JOB_API_H_

#include "qpl/c_api/status.h"
#include "qpl/c_api/defs.h"
#include "qpl/c_api/huffman_table.h"
#include "qpl/c_api/dictionary.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup TASK_API Job API
 * @ingroup JOB_API
 * @{
 */

/**
 * @brief @ref qpl_job extension that holds internal buffers and context for @ref qpl_operation
 */
struct qpl_aux_data {
    uint8_t    *compress_state_ptr;      /**< @ref qpl_op_compress operation state */
    uint8_t    *decompress_state_ptr;    /**< @ref qpl_op_decompress operation state */
    uint8_t    *analytics_state_ptr;     /**< Analytics @ref qpl_operation buffers */
    uint8_t    *middle_layer_buffer_ptr; /**< Internal middle-level layer buffer */
    uint8_t    *hw_state_ptr;            /**< Hardware path execution context */
    qpl_path_t path;                     /**< @ref qpl_path_t marker */
};

typedef struct qpl_aux_data qpl_data; /**< Hidden internal state structure */

/**
 * @brief Defines the general Intel QPL JOB API structure to perform task configuration.
 */
typedef struct {
    uint8_t  *next_in_ptr;    /**< Next input byte */
    uint32_t available_in;    /**< Number of bytes available at next_in_ptr */
    uint32_t total_in;        /**< Total number of bytes read so far */

    uint8_t  *next_out_ptr;    /**< Next output byte */
    uint32_t available_out;    /**< Number of bytes available at next_out_ptr */
    uint32_t total_out;        /**< Total number of bytes written so far */

    qpl_operation          op;                 /**< Intel QPL operation */
    uint32_t               flags;              /**< Auxiliary operation flags - see below */
    uint32_t               crc;                /**< CRC - Input and Output */
    uint32_t               xor_checksum;       /**< Simple XOR check sum */
    uint32_t               last_bit_offset;    /**< Actual bits in the last written byte */
    qpl_compression_levels level;              /**< Compression level - default or high */
    qpl_statistics_mode    statistics_mode;    /**< Represents mode in which deflate should be performed */

    // Tables
    qpl_huffman_table_t   huffman_table;      /**< Huffman table for compression */

    qpl_dictionary *dictionary;    /**< The dictionary used for compression / decompression */

    // Fields for indexing
    qpl_mini_block_size mini_block_size;    /**< Index block (mini-block) size */
    uint64_t            *idx_array;         /**< Index array address */
    uint32_t            idx_max_size;       /**< Size of index array */
    uint32_t            idx_num_written;    /**< Number of generated indexes */

    // Advanced decompress fields
    uint8_t decomp_end_processing;    /**< Value is qpl_decomp_end_proc */
    uint8_t ignore_start_bits;        /**< 0-7 - a number of bits to skip at the start of the 1st byte */
    uint8_t ignore_end_bits;          /**< 0-7 - a number of bits to ignore at the end of the last byte */

    // CRC64 fields
    uint64_t crc64_poly;    /**< Polynomial used for the crc64 operation */
    uint64_t crc64;         /**< Initial and final CRC value for the crc64 operation */

    // Filter Function Fields
    uint8_t    *next_src2_ptr;        /**< Pointer to source-2 data. Updated value is returned */
    uint32_t   available_src2;        /**< Number of valid bytes of source-2 data */
    uint32_t   src1_bit_width;        /**< Source-1 bit width for Analytics. Valid values are 1-32 */
    uint32_t   src2_bit_width;        /**< Source-2 bit width for Analytics. Valid values are 1-32 */
    uint32_t   num_input_elements;    /**< Number of input elements for Analytics */

    /**
     * Output bit width enumeration. Valid values are nominal, 8-, 16-, or 32-bits
     */
    qpl_out_format out_bit_width;

    /**
     * Low parameter for operations extract or scan
     */
    uint32_t param_low;

    /**
     * High parameter for operations extract or scan
     */
    uint32_t param_high;

    /**
     * Number of initial bytes to be dropped at the start of the Analytics portion of the pipeline
     */
    uint32_t drop_initial_bytes;

    /**
     * The index of initial output element from Analytics. This affects modified bit-vector output
     * and the bit-vector aggregate values
     */
    uint32_t initial_output_index;

    /**
     * Enumeration of what parser to use to parse Analytics source-1 data
     */
    qpl_parser parser;

    // Filter Aggregate Values
    uint32_t first_index_min_value;    /**< Output aggregate value - index of the first min value */
    uint32_t last_index_max_value;     /**< Output aggregate value - index of the last max value */
    uint32_t sum_value;                /**< Output aggregate value - sum of all values */

    // NUMA ID
    int32_t numa_id; /**< ID of the NUMA. Set it to -1 for auto detecting */

    // storage for auxiliary data
    qpl_data data_ptr;    /**< Internal memory buffers & structures for all Intel QPL operations */
} qpl_job;

/** @} */

/**
 * @defgroup JOB_API_FUNCTIONS Functions
 * @ingroup JOB_API
 * @{
 * @brief Intel® Query Processing Library (Intel® QPL) C API
 */

/**
 * @brief Calculates the amount of memory, in bytes, required for the qpl_job structure.
 *
 * @param[in]   qpl_path      type of implementation path to use - @ref qpl_path_auto,
 *                            @ref qpl_path_hardware or @ref qpl_path_software
 * @param[out]  job_size_ptr  a pointer to uint32_t, where the qpl_job size (in bytes) is stored
 *
 * @note Some kind of dispatching is done at this stage - in absence of hardware,
 *       it will require significantly more memory for internal buffers
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_PATH_ERR.
 */
QPL_API(qpl_status, qpl_get_job_size, (qpl_path_t qpl_path, uint32_t * job_size_ptr))

/**
 * @brief Initializes the qpl_job structure and ensures proper alignment of internal structures.
 * This API should be called only once, after a new @ref qpl_job object is allocated.
 *
 * @param[in]      qpl_path     type of implementation path to use - @ref qpl_path_auto,
 *                              @ref qpl_path_hardware or @ref qpl_path_software
 * @param[in,out]  qpl_job_ptr  a pointer to the @ref qpl_job structure
 *
 * @warning Memory for qpl_job structure must be allocated at the application side. Size (in bytes)
 * must be obtained with the @ref qpl_get_job_size function.
 *
 * @note qpl_job is an alias to the @ref qpl_job structure - must contain additional internal memory buffers for
 * SW path of compression/decompression/etc.
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_PATH_ERR;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_init_job, (qpl_path_t qpl_path, qpl_job * qpl_job_ptr))

/**
 * @brief Parses the qpl_job structure and forms the corresponding processing functions pipeline.
 *
 * @param[in,out]  qpl_job_ptr  Pointer to the initialized @ref qpl_job structure
 *
 * @return One of statuses presented in the @ref qpl_status
 */
QPL_API(qpl_status, qpl_execute_job, (qpl_job * qpl_job_ptr))

/**
 * @brief Parses the qpl_job structure and forms the corresponding processing functions pipeline.
 *        In case of software solution, it is an alias for execute_job.
 *
 * @param[in,out]  qpl_job_ptr  Pointer to the initialized @ref qpl_job structure
 *
 * @return One of statuses presented in the @ref qpl_status
 *
 */
QPL_API(qpl_status, qpl_submit_job, (qpl_job * qpl_job_ptr))

/**
 * @brief Waits for the end of @ref qpl_job processing. (waits until the job is completed)
 *
 * @param[in,out]  qpl_job_ptr  Pointer to the initialized @ref qpl_job structure
 *
 * @return One of statuses presented in the @ref qpl_status
 *
 */
QPL_API(qpl_status, qpl_wait_job, (qpl_job * qpl_job_ptr))

/**
 * @brief Checks the status of @ref qpl_job processing. (can be queried periodically to check the status
 *        of the qpl_submit_job)
 *
 * @param[in,out]  qpl_job_ptr  Pointer to the initialized @ref qpl_job structure
 *
 * @return One of statuses presented in the @ref qpl_status
 */
QPL_API(qpl_status, qpl_check_job, (qpl_job * qpl_job_ptr))

/**
 * @brief Completes @ref qpl_job lifecycle: disconnects from the internal library context, frees internal resources.
 *
 * @param qpl_job_ptr Pointer to the initialized @ref qpl_job structure
 *
 * @return ne of statuses presented in the @ref qpl_status
 */
QPL_API(qpl_status, qpl_fini_job, (qpl_job * qpl_job_ptr))

/** @} */

#ifdef __cplusplus
}
#endif

#endif //QPL_JOB_API_H_
