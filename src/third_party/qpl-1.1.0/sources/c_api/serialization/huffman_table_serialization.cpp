/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Serialization API for Huffman table
 */

#include "qpl/c_api/huffman_table.h"
#include "huffman_table_serialization.hpp"
#include "compression/huffman_table/huffman_table.hpp"
#include "compression/huffman_table/serialization_utils.hpp"
#include "compression/huffman_table/huffman_table_utils.hpp" // qpl_{de}compression_huffman_table
#include "util/checkers.hpp" // OWN_QPL_CHECK_STATUS
#include "own_checkers.h"    // QPL_BADARG_RET

extern "C" {

/*
 * Storage scheme for a serialized table:
 * |meta structure|compression table|decompression table|
 *
 * Whether we have both tables or just one in the buffer is determined
 * by value stored in meta.type (combined, compression or decompression table).
 *
 * Current (de)compression_table_size is taking alignment/padding into account,
 * as it is sizeof(struct) rather than sum (sizeof(struct.member)).
 * This could be improved in the future if necessary by introducing
 * size_t flatten_table_size(const T &table) {}, with T covering various internal structures
 * such as qplc_huffman_table_default_format, isal_hufftables, hw_compression_huffman_table, etc.
 */

/**
 * @brief Function to get size of the table to be serialized.
 */
qpl_status qpl_huffman_table_get_serialized_size(const qpl_huffman_table_t table,
                                                 const serialization_options_t options,
                                                 size_t *const size_ptr) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table))
    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(size_ptr))
    QPL_BADARG_RET(options.format > serialization_raw, QPL_STS_SERIALIZATION_FORMAT_ERROR)

    if (options.format != serialization_raw)
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;

    auto meta_ptr = reinterpret_cast<huffman_table_meta_t*>(table);
    if (meta_ptr->algorithm != compression_algorithm_e::deflate)
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;

    // obtain size for storing meta data
    size_t meta_size = 0;
    qpl::ml::serialization::get_meta_size(*meta_ptr, &meta_size);

    // todo: consider moving to a separate function to get table sizes,
    //       might be useful for the future, if we decide to use actual
    //       flatten size vs sizeof(struct) or if the internal table impl
    //       would be path or anything else dependent

    // obtain size for for storing compression/decompression tables
    size_t tables_size = 0;
    switch (meta_ptr->type) {
        case huffman_table_type_e::compression:
            tables_size += sizeof(qpl_compression_huffman_table);
            break;

        case huffman_table_type_e::decompression:
            tables_size += sizeof(qpl_decompression_huffman_table);
            break;

        default:
            tables_size += sizeof(qpl_compression_huffman_table) + sizeof(qpl_decompression_huffman_table);
    }

    *size_ptr = meta_size + tables_size;

    return QPL_STS_OK;
}

/**
 * @brief Function that serializes Huffman table and store the result into a buffer.
 */
qpl_status qpl_huffman_table_serialize(const qpl_huffman_table_t table,
                                       uint8_t *const stream_buffer,
                                       const size_t stream_buffer_size,
                                       const serialization_options_t options) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table))
    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(stream_buffer))
    QPL_BADARG_RET(options.format > serialization_raw, QPL_STS_SERIALIZATION_FORMAT_ERROR)
    if (stream_buffer_size == 0)
        return QPL_STS_SIZE_ERR;

    if (options.format != serialization_raw)
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;

    auto meta_ptr = reinterpret_cast<huffman_table_meta_t*>(table);
    if (meta_ptr->algorithm != compression_algorithm_e::deflate)
        return QPL_STS_NOT_SUPPORTED_MODE_ERR;

    // todo: move impl to a special namespace to reflect meta struct version,
    // to accommodate future implementations
    // e.g. qpl::ml::serialization::v1::serialize_meta
    qpl::ml::serialization::serialize_meta(*meta_ptr, stream_buffer);

    size_t offset = 0;
    qpl::ml::serialization::get_meta_size(*meta_ptr, &offset);

    if (meta_ptr->algorithm == compression_algorithm_e::deflate) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(table);

        return static_cast<qpl_status>(table_impl->write_to_stream(stream_buffer + offset));
    }

    if (meta_ptr->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(table);

        return static_cast<qpl_status>(table_impl->write_to_stream(stream_buffer + offset));
    }

    return QPL_STS_OK;
}

/**
 * @brief Function that creates and initializes Huffman table using the memory buffer,
 * that stores previously serialized table.
 */
qpl_status qpl_huffman_table_deserialize(const uint8_t *const stream_buffer,
                                         const size_t stream_buffer_size,
                                         allocator_t allocator,
                                         qpl_huffman_table_t *table_ptr) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table_ptr))
    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(stream_buffer))
    if (stream_buffer_size == 0)
        return QPL_STS_SIZE_ERR;

    allocator_t meta_allocator = details::get_allocator(allocator);

    auto allocated_size = sizeof(huffman_table_meta_t);
    auto buffer = meta_allocator.allocator(allocated_size);

    if (!buffer) {
        return QPL_STS_OBJECT_ALLOCATION_ERR;
    }

    // Temporary meta structure to get information required for creation
    // and initialization of the Huffman table, will be discarded at the end
    huffman_table_meta_t *meta_ptr = new (buffer) huffman_table_meta_t();

    // todo: move impl to a special namespace to reflect meta struct version,
    // to accommodate future implementations
    // e.g. qpl::ml::serialization::v1::deserialize_meta
    qpl::ml::serialization::deserialize_meta(stream_buffer, *meta_ptr);

    // creation
    qpl_status status = QPL_STS_OK;
    if (meta_ptr->algorithm == compression_algorithm_e::deflate) {
        status = qpl_deflate_huffman_table_create((qpl_huffman_table_type_e) meta_ptr->type,
                                                  (qpl_path_t) meta_ptr->path,
                                                  allocator,
                                                  table_ptr);
    }
    if (meta_ptr->algorithm == compression_algorithm_e::huffman_only) {
        status = qpl_huffman_only_table_create((qpl_huffman_table_type_e) meta_ptr->type,
                                               (qpl_path_t) meta_ptr->path,
                                               allocator,
                                               table_ptr);
    }
    if (status != QPL_STS_OK) {
        std::destroy_at(meta_ptr);
        meta_allocator.deallocator(buffer);

        return QPL_STS_OBJECT_ALLOCATION_ERR;
    }

    // initialization
    size_t offset = 0;
    qpl::ml::serialization::get_meta_size(*meta_ptr, &offset);

    if (meta_ptr->algorithm == compression_algorithm_e::deflate) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(*table_ptr);

        status = static_cast<qpl_status>(table_impl->init_with_stream(stream_buffer + offset));
    }
    if (meta_ptr->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(*table_ptr);

        status = static_cast<qpl_status>(table_impl->init_with_stream(stream_buffer + offset));
    }

    std::destroy_at(meta_ptr);
    meta_allocator.deallocator(buffer);

    return status;
}

}
