/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#include "util/memory.hpp"
#include "util/checkers.hpp"
#include "own_checkers.h"
#include "huffman_table.hpp"
#include "compression/huffman_table/huffman_table_utils.hpp"
#include "compression/huffman_table/huffman_table.hpp"

extern "C" {

qpl_status qpl_deflate_huffman_table_create(const qpl_huffman_table_type_e type,
                                            const qpl_path_t path,
                                            const allocator_t allocator,
                                            qpl_huffman_table_t *huffman_table) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;
    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(huffman_table))
    QPL_BADARG_RET(path > qpl_path_software, QPL_STS_PATH_ERR)
    QPL_BADARG_RET(type > decompression_table_type, QPL_STS_HUFFMAN_TABLE_TYPE_ERROR)

    allocator_t table_allocator = details::get_allocator(allocator);

    auto allocated_size = sizeof(huffman_table_t<compression_algorithm_e::deflate>);
    auto buffer = table_allocator.allocator(allocated_size);

    if (!buffer) {
        return QPL_STS_OBJECT_ALLOCATION_ERR;
    }

    huffman_table_t<compression_algorithm_e::deflate>* huffman_table_ptr = new (buffer) huffman_table_t<compression_algorithm_e::deflate>();

    auto status = huffman_table_ptr->create((huffman_table_type_e) type,
                                            (execution_path_t) path,
                                            (allocator_t) allocator);
    if (status != status_list::ok) {
        std::destroy_at(huffman_table_ptr);
        table_allocator.deallocator(buffer);

        *huffman_table = nullptr;
        return QPL_STS_OBJECT_ALLOCATION_ERR;
    }

    *huffman_table = reinterpret_cast<qpl_huffman_table_t>(huffman_table_ptr);

    return QPL_STS_OK;
}

qpl_status qpl_huffman_only_table_create(const qpl_huffman_table_type_e type,
                                         const qpl_path_t path,
                                         const allocator_t allocator,
                                         qpl_huffman_table_t *huffman_table) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;
    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(huffman_table))
    QPL_BADARG_RET(path > qpl_path_software, QPL_STS_PATH_ERR)
    QPL_BADARG_RET(type > decompression_table_type, QPL_STS_HUFFMAN_TABLE_TYPE_ERROR)

    *huffman_table = nullptr;

    allocator_t table_allocator = details::get_allocator(allocator);

    auto allocated_size = sizeof(huffman_table_t<compression_algorithm_e::huffman_only>);
    auto buffer = table_allocator.allocator(allocated_size);

    if (!buffer) {
        return QPL_STS_OBJECT_ALLOCATION_ERR;
    }

    huffman_table_t<compression_algorithm_e::huffman_only>* huffman_table_ptr = new (buffer) huffman_table_t<compression_algorithm_e::huffman_only>();

    auto status = huffman_table_ptr->create((huffman_table_type_e) type,
                                            (execution_path_t) path,
                                            (allocator_t) allocator);
    if (status != status_list::ok) {
        std::destroy_at(huffman_table_ptr);
        table_allocator.deallocator(buffer);

        *huffman_table = nullptr;
        return QPL_STS_OBJECT_ALLOCATION_ERR;
    }

    *huffman_table = reinterpret_cast<qpl_huffman_table_t>(huffman_table_ptr);

    return QPL_STS_OK;
}

qpl_status qpl_huffman_table_destroy(qpl_huffman_table_t table) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;
    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table))

    auto meta = reinterpret_cast<huffman_table_meta_t*>(table);

    if (meta->algorithm == compression_algorithm_e::deflate) {
        auto table_impl       = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(table);
        allocator_t allocator = table_impl->get_internal_allocator();

        std::destroy_at(table_impl);
        allocator.deallocator(table);
    } else if (meta->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl       = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(table);
        allocator_t allocator = table_impl->get_internal_allocator();

        std::destroy_at(table_impl);
        allocator.deallocator(table);
    }

    return QPL_STS_OK;
}

qpl_status qpl_huffman_table_init_with_histogram(qpl_huffman_table_t table,
                                                 const qpl_histogram *const histogram_ptr) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table, histogram_ptr));

    QPL_BADARG_RET(histogram_ptr->reserved_literal_lengths[0] ||
                   histogram_ptr->reserved_literal_lengths[1] ||
                   histogram_ptr->reserved_distances[0] ||
                   histogram_ptr->reserved_distances[1],
                   QPL_STS_INVALID_PARAM_ERR);

    auto meta = reinterpret_cast<huffman_table_meta_t*>(table);

    if (meta->algorithm == compression_algorithm_e::deflate) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(table);
        return static_cast<qpl_status>(table_impl->init(*histogram_ptr));
    }

    if (meta->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(table);
        return static_cast<qpl_status>(table_impl->init(*histogram_ptr));
    }

    return QPL_STS_OK;
}

qpl_status qpl_huffman_table_init_with_triplets(qpl_huffman_table_t table,
                                                const qpl_huffman_triplet *const triplet_ptr,
                                                const uint32_t triplet_count) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table, triplet_ptr));

    auto meta = reinterpret_cast<huffman_table_meta_t*>(table);

    QPL_BADARG_RET(meta->flags & QPL_DEFLATE_REPRESENTATION, QPL_STS_INVALID_HUFFMAN_TABLE_ERR);
    QPL_BADARG_RET(meta->flags & QPL_HUFFMAN_ONLY_REPRESENTATION && triplet_count != 256,
                   QPL_STS_SIZE_ERR);

    if (meta->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(table);

        return static_cast<qpl_status>(table_impl->init(reinterpret_cast<const qpl_triplet *>(triplet_ptr),
                                                        triplet_count));
    }

    return QPL_STS_OK;
}

qpl_status qpl_huffman_table_init_with_other(qpl_huffman_table_t table,
                                             const qpl_huffman_table_t other) {

    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    constexpr auto TABLE_TYPE_FLAG_MASK = QPL_HUFFMAN_ONLY_REPRESENTATION | QPL_DEFLATE_REPRESENTATION;
    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table, other));

    auto first_meta  = reinterpret_cast<huffman_table_meta_t*>(table);
    auto second_meta = reinterpret_cast<huffman_table_meta_t*>(other);

    QPL_BADARG_RET((first_meta->flags & TABLE_TYPE_FLAG_MASK) != (second_meta->flags & TABLE_TYPE_FLAG_MASK),
                   QPL_STS_INVALID_HUFFMAN_TABLE_ERR)

    if (first_meta->algorithm == compression_algorithm_e::deflate) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(table);
        auto other_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(other);

        return static_cast<qpl_status>(table_impl->init(*other_impl));
    }

    if (first_meta->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(table);
        auto other_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(other);

        return static_cast<qpl_status>(table_impl->init(*other_impl));
    }

    return QPL_STS_OK;
}

qpl_status qpl_huffman_table_get_type(qpl_huffman_table_t table,
                                      qpl_huffman_table_type_e *const type_ptr) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    OWN_QPL_CHECK_STATUS(bad_argument::check_for_nullptr(table, type_ptr))

    auto meta = reinterpret_cast<huffman_table_meta_t*>(table);

    *type_ptr = static_cast<qpl_huffman_table_type_e>(meta->type);

    return QPL_STS_OK;
}

// Delete after refactoring
qpl_compression_huffman_table *own_huffman_table_get_compression_table(const qpl_huffman_table_t table) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    auto meta = reinterpret_cast<huffman_table_meta_t*>(table);

    if (meta->algorithm == compression_algorithm_e::deflate) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(table);

        auto c_table = table_impl->compression_huffman_table<execution_path_t::software>();

        return reinterpret_cast<qpl_compression_huffman_table *>(c_table);
    }

    if (meta->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(table);

        auto c_table = table_impl->compression_huffman_table<execution_path_t::software>();

        return reinterpret_cast<qpl_compression_huffman_table *>(c_table);
    }

    return nullptr;
}

//
qpl_decompression_huffman_table *own_huffman_table_get_decompression_table(const qpl_huffman_table_t table) {
    using namespace qpl::ml;
    using namespace qpl::ml::compression;

    auto meta = reinterpret_cast<huffman_table_meta_t*>(table);

    if (meta->algorithm == compression_algorithm_e::deflate) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::deflate>*>(table);

        auto d_table = table_impl->decompression_huffman_table<execution_path_t::software>();

        return reinterpret_cast<qpl_decompression_huffman_table *>(d_table);
    }

    if (meta->algorithm == compression_algorithm_e::huffman_only) {
        auto table_impl = reinterpret_cast<huffman_table_t<compression_algorithm_e::huffman_only>*>(table);

        auto d_table = table_impl->decompression_huffman_table<execution_path_t::software>();

        return reinterpret_cast<qpl_decompression_huffman_table *>(d_table);
    }

    return nullptr;
}

}
