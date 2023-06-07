/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_HUFFMAN_TABLE_H_
#define QPL_HUFFMAN_TABLE_H_

#include <stdint.h>

#include "qpl/c_api/status.h"
#include "qpl/c_api/statistics.h"
#include "qpl/c_api/serialization.h"
#include "qpl/c_api/triplet.h"

/**
 * @defgroup HUFFMAN_TABLE_API Huffman Table API
 * @ingroup JOB_API
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Huffman table::defs
 * @{
 */

/**
 * @typedef qpl_huffman_table_t
 * @brief Special data type that is an opaque pointer to unified compression/decompression table.
 */
typedef struct qpl_huffman_table *qpl_huffman_table_t;

/**
 * @struct allocator_t
 * @brief Structure that describes user-provided allocator.
 */
typedef struct {
    void *(*allocator)(size_t);  /**< Allocation function */
    void (*deallocator)(void *); /**< Deallocation function */
} allocator_t;

/**
 * @enum qpl_huffman_table_type_e
 * @brief Type used to specify whether Huffman table would store compression, decompression or both tables internally.
 */
typedef enum {
    combined_table_type,      /**< @ref qpl_huffman_table_t contains both tables */
    compression_table_type,   /**< @ref qpl_huffman_table_t contains compression table only */
    decompression_table_type, /**< @ref qpl_huffman_table_t contains decompression table only */
} qpl_huffman_table_type_e;

/**
* Allocator used in Intel QPL C API by default
*/
#define DEFAULT_ALLOCATOR_C {malloc, free}

/** @} */

/* --------------------------------------------------------------------------------*/

/**
 * @name Huffman table::Lifetime API
 * @{
 */

/**
 * @brief Creates a @ref qpl_huffman_table_t object for deflate. Allocate and markup of internal structures
 *
 * @param[in] type       @ref qpl_huffman_table_type_e
 * @param[in] path       @ref qpl_path_t
 * @param[in] allocator  @ref allocator_t that must be used
 * @param[out] table_ptr output parameter for created object
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_deflate_huffman_table_create(const qpl_huffman_table_type_e type,
                                            const qpl_path_t path,
                                            const allocator_t allocator,
                                            qpl_huffman_table_t *table_ptr);

/**
 * @brief Creates a @ref qpl_huffman_table_t object for Huffman Only. Allocate and markup of internal structures
 *
 * @param[in]  type      @ref qpl_huffman_table_type_e
 * @param[in]  path      @ref qpl_path_t
 * @param[in]  allocator allocator that must be used
 * @param[out] table_ptr output parameter for created object
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_only_table_create(const qpl_huffman_table_type_e type,
                                         const qpl_path_t path,
                                         const allocator_t allocator,
                                         qpl_huffman_table_t *table_ptr);

/**
 * @brief Destroy an @ref qpl_huffman_table_t object. Deallocates internal structures
 *
 * @param[in,out] table  @ref qpl_huffman_table_t object to destroy
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_destroy(qpl_huffman_table_t table);

/** @} */

/* --------------------------------------------------------------------------------*/

/**
 * @name Huffman table::Initialization API
 * @{
 */

/**
 * @brief Initializes huffman table with provided histogram
 *
 * @param[in,out] table      @ref qpl_huffman_table_t object to init
 * @param[in] histogram_ptr  source statistics
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_init_with_histogram(qpl_huffman_table_t table,
                                                 const qpl_histogram *const histogram_ptr);

/**
 * @brief Initializes huffman table with provided triplets
 *
 * @param[in,out] table @ref qpl_huffman_table_t object to init
 * @param[in] triplet_ptr   user defined triplet huffman codes
 * @param[in] triplet_count huffman codes count
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_init_with_triplets(qpl_huffman_table_t table,
                                                const qpl_huffman_triplet *const triplet_ptr,
                                                const uint32_t triplet_count);

/**
 * @brief Initializes huffman table with information from another table
 *
 * @param[in,out] table @ref qpl_huffman_table_t object to init
 * @param[in] other base @ref qpl_huffman_table_t object
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_init_with_other(qpl_huffman_table_t table,
                                             const qpl_huffman_table_t other);

/** @} */

/* --------------------------------------------------------------------------------*/

/**
 * @name Huffman table::Service API
 * @{
 */

/**
 * @brief Returns type of @ref qpl_huffman_table_t
 *
 * @param[in]  table    source @ref qpl_huffman_table_t object
 * @param[out] type_ptr output parameter for table type according to @ref qpl_huffman_table_type_e
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_get_type(const qpl_huffman_table_t table,
                                      qpl_huffman_table_type_e *const type_ptr);

/** @} */

/* --------------------------------------------------------------------------------*/

/**
 * @name Huffman table::Serialization API
 * @{
 * @brief To keep user space on filesystem we can serialize and pack qpl_huffman_table_t into
 * raw or more compact format.
 */

/**
 * @brief API to get size of the table to be serialized.
 * @note Serialization is only supported for serialization_raw format.
 *
 * @param[in]  table    @ref qpl_huffman_table_t object to serialize
 * @param[in]  options  @ref serialization_options_t
 * @param[out] size_ptr output parameter for size
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_get_serialized_size(const qpl_huffman_table_t table,
                                                 const serialization_options_t options,
                                                 size_t *const size_ptr);

/**
 * @brief Serializes qpl_huffman_table_t object.
 * @note Serialization is only supported for serialization_raw format.
 *
 * @param[in] table @ref qpl_huffman_table_t object to serialize
 * @param[out] dump_buffer_ptr serialized object buffer
 * @param[in] dump_buffer_size serialized object buffer size
 * @param[in] options @ref serialization_options_t
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_serialize(const qpl_huffman_table_t table,
                                       uint8_t *const dump_buffer_ptr,
                                       const size_t dump_buffer_size,
                                       const serialization_options_t options);

/**
 * @brief Deserializes previously serialized huffman table
 *
 * @param[in] dump_buffer_ptr serialized object buffer
 * @param[in] dump_buffer_size serialized object buffer size
 * @param[in] allocator allocator that must be used
 * @param[out] table_ptr output parameter for created object
 *
 * @return status from @ref qpl_status
 */
qpl_status qpl_huffman_table_deserialize(const uint8_t *const dump_buffer_ptr,
                                         const size_t dump_buffer_size,
                                         allocator_t allocator,
                                         qpl_huffman_table_t *table_ptr);

/** @} */

#ifdef __cplusplus
}
#endif

/** @} */

#endif //QPL_HUFFMAN_TABLE_H_
