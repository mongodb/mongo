/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#include "memory"
#include "array" // std::array
#include <cstring>

#include "huffman_table.hpp"
#include "compression/huffman_table/deflate_huffman_table.hpp"
#include "compression/huffman_table/inflate_huffman_table.hpp"
#include "util/util.hpp"
#include "compression/huffman_table/huffman_table_utils.hpp" // qpl::ml::compression qpl_triplet

namespace qpl::ml::compression {

template<>
qpl_ml_status huffman_table_t<compression_algorithm_e::deflate>::create(huffman_table_type_e type, execution_path_t path, allocator_t allocator) {
    m_meta.algorithm = compression_algorithm_e::deflate;
    m_meta.type      = type;
    m_meta.path      = path;
    m_meta.flags     = details::get_path_flags(path) | QPL_DEFLATE_REPRESENTATION;
    m_meta.version   = LAST_VERSION;

    m_is_initialized = false;

    m_allocator.allocator   = allocator.allocator;
    m_allocator.deallocator = allocator.deallocator;

    auto   allocated_size             = 0u;
    size_t decompression_table_offset = 0u;

    switch (type) {
        case huffman_table_type_e::compression:
            allocated_size += sizeof(qpl_compression_huffman_table);
            break;

        case huffman_table_type_e::decompression:
            allocated_size += sizeof(qpl_decompression_huffman_table);
            break;

        default:
            allocated_size += sizeof(qpl_compression_huffman_table) + sizeof(qpl_decompression_huffman_table);
            decompression_table_offset = sizeof(qpl_compression_huffman_table);
    }

    allocator_t table_allocator = details::get_allocator(allocator);
    auto buffer = table_allocator.allocator(allocated_size);
    if (!buffer) return status_list::nullptr_error;

    memset(buffer, 0u, allocated_size);

    m_tables_buffer = std::unique_ptr<uint8_t[], void(*)(void*)>(reinterpret_cast<uint8_t*>(buffer),
                                                                 table_allocator.deallocator);

    switch (type) {
        case huffman_table_type_e::compression:
            m_c_huffman_table = m_tables_buffer.get();
            m_d_huffman_table = nullptr;
            break;

        case huffman_table_type_e::decompression:
            m_c_huffman_table = nullptr;
            m_d_huffman_table = m_tables_buffer.get() + decompression_table_offset;
            break;

        default:
            m_c_huffman_table = m_tables_buffer.get();
            m_d_huffman_table = m_tables_buffer.get() + decompression_table_offset;
    }

    return status_list::ok;
}

template<>
qpl_ml_status huffman_table_t<compression_algorithm_e::huffman_only>::create(huffman_table_type_e type, execution_path_t path, allocator_t allocator) {
    m_meta.algorithm = compression_algorithm_e::huffman_only;
    m_meta.type      = type;
    m_meta.path      = path;
    m_meta.flags     = details::get_path_flags(path) | QPL_HUFFMAN_ONLY_REPRESENTATION;
    m_meta.version   = LAST_VERSION;

    m_is_initialized = false;

    m_allocator.allocator   = allocator.allocator;
    m_allocator.deallocator = allocator.deallocator;

    auto   allocated_size             = 0u;
    size_t decompression_table_offset = 0u;

    switch (type) {
        case huffman_table_type_e::compression:
            allocated_size += sizeof(qpl_compression_huffman_table);
            break;
        case huffman_table_type_e::decompression:
            allocated_size += sizeof(qpl_decompression_huffman_table);
            break;
        default:
            allocated_size += sizeof(qpl_compression_huffman_table) + sizeof(qpl_decompression_huffman_table);
            decompression_table_offset = sizeof(qpl_compression_huffman_table);
    }

    allocator_t table_allocator = details::get_allocator(allocator);
    auto buffer = table_allocator.allocator(allocated_size);
    if (!buffer) return status_list::nullptr_error;

    memset(buffer, 0u, allocated_size);

    m_tables_buffer = std::unique_ptr<uint8_t[], void(*)(void*)>(reinterpret_cast<uint8_t*>(buffer),
                                                                 table_allocator.deallocator);

    switch (type) {
        case huffman_table_type_e::compression:
            m_c_huffman_table = m_tables_buffer.get();
            m_d_huffman_table = nullptr;
            break;

        case huffman_table_type_e::decompression:
            m_c_huffman_table = nullptr;
            m_d_huffman_table = m_tables_buffer.get() + decompression_table_offset;
            break;

        default:
            m_c_huffman_table = m_tables_buffer.get();
            m_d_huffman_table = m_tables_buffer.get() + decompression_table_offset;
    }

    return status_list::ok;
}

template <compression_algorithm_e algorithm>
qpl_ml_status huffman_table_t<algorithm>::init(const qpl_histogram &histogram_ptr) noexcept {
    if (m_c_huffman_table) {
        auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

        auto status = compression::huffman_table_init(*c_table,
                                                      histogram_ptr.literal_lengths,
                                                      histogram_ptr.distances,
                                                      m_meta.flags);
        if (status) {
            return status;
        }
    }

    if (m_d_huffman_table) {
        if (m_c_huffman_table && m_meta.flags & QPL_DEFLATE_REPRESENTATION) {
            auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);
            auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

            auto status = compression::huffman_table_convert(*c_table, *d_table, m_meta.flags);
            if (status) {
                return status;
            }
        } else {
            return status_list::not_supported_err;
        }
    }

    m_is_initialized = true;

    return status_list::ok;
}

template
qpl_ml_status huffman_table_t<compression_algorithm_e::deflate>::init(const qpl_histogram &histogram_ptr) noexcept;

template
qpl_ml_status huffman_table_t<compression_algorithm_e::huffman_only>::init(const qpl_histogram &histogram_ptr) noexcept;


template <compression_algorithm_e algorithm>
qpl_ml_status huffman_table_t<algorithm>::init(const qpl_triplet *triplet_ptr, const size_t count) noexcept {
    if (m_c_huffman_table) {
        auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

        auto status = compression::huffman_table_init(*c_table, triplet_ptr, count, m_meta.flags);
        if (status) {
            return static_cast<qpl_status>(status);
        }
    }

    if (m_d_huffman_table) {
        auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

        auto status = compression::huffman_table_init(*d_table, triplet_ptr, count, m_meta.flags);
        if (status) {
            return static_cast<qpl_status>(status);
        }
    }

    m_is_initialized = true;

    return status_list::ok;
}

template
qpl_ml_status huffman_table_t<compression_algorithm_e::deflate>::init(const qpl_triplet *triplet_ptr,
                                                                      const size_t count) noexcept;

template
qpl_ml_status huffman_table_t<compression_algorithm_e::huffman_only>::init(const qpl_triplet *triplet_ptr,
                                                                           const size_t count) noexcept;


template<>
qpl_ml_status huffman_table_t<compression_algorithm_e::deflate>::init(const huffman_table_t<compression_algorithm_e::deflate> &other) noexcept {
    if (m_meta.type == huffman_table_type_e::decompression) {
        auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(other.m_c_huffman_table);
        auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

        auto status = compression::huffman_table_convert(*c_table,
                                                         *d_table,
                                                         m_meta.flags);
        if (status) {
            return static_cast<qpl_status>(status);
        }
    } else {
        return status_list::not_supported_err;
    }

    m_is_initialized = true;

    return status_list::ok;
}

template<>
qpl_ml_status huffman_table_t<compression_algorithm_e::huffman_only>::init(const huffman_table_t<compression_algorithm_e::huffman_only> &other) noexcept {
    if (m_meta.type == huffman_table_type_e::decompression) {
        constexpr auto     QPL_HUFFMAN_CODE_BIT_LENGTH = 15u;
        constexpr uint16_t code_mask                   = (1u << QPL_HUFFMAN_CODE_BIT_LENGTH) - 1u;

        std::array<qpl::ml::compression::qpl_triplet, 256> triplets_tmp{};

        auto literals_matches_table_ptr = reinterpret_cast<uint32_t *>(other.m_c_huffman_table);

        // Prepare triplets
        for (uint16_t i = 0u; i < 256u; i++) {
            triplets_tmp[i].code        = literals_matches_table_ptr[i] & code_mask;
            triplets_tmp[i].code_length = literals_matches_table_ptr[i] >> QPL_HUFFMAN_CODE_BIT_LENGTH;
            triplets_tmp[i].character   = static_cast<uint8_t>(i);
        }

        auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

        auto status = compression::huffman_table_init(*d_table,
                                                      triplets_tmp.data(),
                                                      triplets_tmp.size(),
                                                      m_meta.flags);
        if (status) {
            return static_cast<qpl_status>(status);
        }
    } else {
        return status_list::not_supported_err;
    }

    m_is_initialized = true;

    return status_list::ok;
}

// function to read content of a buffer
// and initialize compression and decompression tables from this data
// currently used for deserialization
template <compression_algorithm_e algorithm>
qpl_ml_status huffman_table_t<algorithm>::init_with_stream(const uint8_t *const buffer) noexcept {

    if (m_meta.algorithm == compression_algorithm_e::deflate) {

        size_t offset = 0;

        if (m_c_huffman_table) {
            auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

            auto status = compression::huffman_table_init_with_stream(*c_table, buffer, m_meta.flags);
            if (status) {
                return static_cast<qpl_status>(status);
            }

            offset += sizeof(qpl_compression_huffman_table);
        }

        if (m_d_huffman_table) {
            auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

            auto status = compression::huffman_table_init_with_stream(*d_table, buffer + offset, m_meta.flags);
            if (status) {
                return static_cast<qpl_status>(status);
            }
        }
    }
    else {
        return status_list::not_supported_err;
    }

    m_is_initialized = true;

    return status_list::ok;
}

template
qpl_ml_status huffman_table_t<compression_algorithm_e::deflate>::init_with_stream(const uint8_t *const buffer) noexcept;

template
qpl_ml_status huffman_table_t<compression_algorithm_e::huffman_only>::init_with_stream(const uint8_t *const buffer) noexcept;

// function to write content of compression and decompression tables
// into buffer, currently used for serialization
template <compression_algorithm_e algorithm>
qpl_ml_status huffman_table_t<algorithm>::write_to_stream(uint8_t *buffer) const noexcept {

    if (m_meta.algorithm == compression_algorithm_e::deflate) {

        size_t offset = 0;

        if (m_c_huffman_table) {
            auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

            auto status = compression::huffman_table_write_to_stream(*c_table, buffer, m_meta.flags);
            if (status) {
                return static_cast<qpl_status>(status);
            }

            offset += sizeof(qpl_compression_huffman_table);
        }

        if (m_d_huffman_table) {
            auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

            auto status = compression::huffman_table_write_to_stream(*d_table, buffer + offset, m_meta.flags);
            if (status) {
                return static_cast<qpl_status>(status);
            }
        }
    }
    else {
        return status_list::not_supported_err;
    }

    return status_list::ok;
}

template
qpl_ml_status huffman_table_t<compression_algorithm_e::deflate>::write_to_stream(uint8_t *buffer) const noexcept;

template
qpl_ml_status huffman_table_t<compression_algorithm_e::huffman_only>::write_to_stream(uint8_t *buffer) const noexcept;

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::compression_huffman_table<execution_path_t::software>() const noexcept {
    return m_c_huffman_table;
}

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::compression_huffman_table<execution_path_t::software>() const noexcept {
    return m_c_huffman_table;
}

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::compression_huffman_table<execution_path_t::hardware>() const noexcept {
    return m_c_huffman_table;
}

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::compression_huffman_table<execution_path_t::hardware>() const noexcept {
    return m_c_huffman_table;
}

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::decompression_huffman_table<execution_path_t::software>() const noexcept {
    return m_d_huffman_table;
}

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::decompression_huffman_table<execution_path_t::software>() const noexcept {
    return m_d_huffman_table;
}

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::decompression_huffman_table<execution_path_t::hardware>() const noexcept {
    return m_d_huffman_table;
}

template<> template<>
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::decompression_huffman_table<execution_path_t::hardware>() const noexcept {
    return m_d_huffman_table;
}

template <compression_algorithm_e algorithm>
bool huffman_table_t<algorithm>::is_initialized() const noexcept {
    return m_is_initialized;
}

template
bool huffman_table_t<compression_algorithm_e::deflate>::is_initialized() const noexcept;

template
bool huffman_table_t<compression_algorithm_e::huffman_only>::is_initialized() const noexcept;

template<>
bool huffman_table_t<compression_algorithm_e::deflate>::is_equal(const huffman_table_t<compression_algorithm_e::deflate> &other) const noexcept {

    bool c_status = true;
    if (m_c_huffman_table) {

        auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);
        auto other_c_table = reinterpret_cast<qpl_compression_huffman_table*>(other.m_c_huffman_table);

        c_status = compression::is_equal(*c_table, *other_c_table);
    }

    bool d_status = true;
    if (m_d_huffman_table) {
        auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);
        auto other_d_table = reinterpret_cast<qpl_decompression_huffman_table*>(other.m_d_huffman_table);

        d_status = compression::is_equal(*d_table, *other_d_table);
    }

    return (c_status && d_status);
}

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_lookup_table_buffer_ptr() const noexcept {
    auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

    return reinterpret_cast<uint8_t *>(&d_table->lookup_table_buffer);
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_lookup_table_buffer_ptr() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_lookup_table_buffer_ptr() const noexcept;

template <compression_algorithm_e algorithm>
void *huffman_table_t<algorithm>::get_aecs_decompress() const noexcept {
    using namespace qpl::ml::compression;

    auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

    auto sw_flattened_table_ptr     = reinterpret_cast<uint8_t *>(&d_table->sw_flattened_table);
    auto hw_decompression_state_ptr = reinterpret_cast<uint8_t *>(&d_table->hw_decompression_state);
    auto deflate_header_buffer_ptr  = reinterpret_cast<uint8_t *>(&d_table->deflate_header_buffer);
    auto lookup_table_buffer_ptr    = reinterpret_cast<uint8_t *>(&d_table->lookup_table_buffer);

    class decompression_huffman_table decompression_table(sw_flattened_table_ptr,
                                                          hw_decompression_state_ptr,
                                                          deflate_header_buffer_ptr,
                                                          lookup_table_buffer_ptr);

    return decompression_table.get_hw_decompression_state();
}

template
void *huffman_table_t<compression_algorithm_e::deflate>::get_aecs_decompress() const noexcept;

template
void *huffman_table_t<compression_algorithm_e::huffman_only>::get_aecs_decompress() const noexcept;


template <compression_algorithm_e algorithm>
uint32_t *huffman_table_t<algorithm>::get_literals_lengths_table_ptr() const noexcept {
    using namespace qpl::ml::compression;

    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    auto sw_compression_table =
                 reinterpret_cast<qplc_huffman_table_default_format *>(&c_table->sw_compression_table_data);

    return sw_compression_table->literals_matches;
}

template
uint32_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_literals_lengths_table_ptr() const noexcept;

template
uint32_t *huffman_table_t<compression_algorithm_e::deflate>::get_literals_lengths_table_ptr() const noexcept;

template <compression_algorithm_e algorithm>
uint32_t *huffman_table_t<algorithm>::get_offsets_table_ptr() const noexcept {
    using namespace qpl::ml::compression;
    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    auto sw_compression_table =
                 reinterpret_cast<qplc_huffman_table_default_format *>(&c_table->sw_compression_table_data);

    return sw_compression_table->offsets;
}

template
uint32_t *huffman_table_t<compression_algorithm_e::deflate>::get_offsets_table_ptr() const noexcept;

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_deflate_header_ptr() const noexcept {
    using namespace qpl::ml::compression;
    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    return reinterpret_cast<deflate_header *>(&c_table->deflate_header_buffer)->data;
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_deflate_header_ptr() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_deflate_header_ptr() const noexcept;

template <compression_algorithm_e algorithm>
uint32_t huffman_table_t<algorithm>::get_deflate_header_bits_size() const noexcept {
    using namespace qpl::ml::compression;
    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    return reinterpret_cast<deflate_header *>(&c_table->deflate_header_buffer)->header_bit_size;
}

template
uint32_t huffman_table_t<compression_algorithm_e::deflate>::get_deflate_header_bits_size() const noexcept;

template <compression_algorithm_e algorithm>
void huffman_table_t<algorithm>::set_deflate_header_bits_size(const uint32_t header_bits) noexcept {
    using namespace qpl::ml::compression;
    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    reinterpret_cast<deflate_header *>(&c_table->deflate_header_buffer)->header_bit_size = header_bits;
}

template
void huffman_table_t<compression_algorithm_e::deflate>::set_deflate_header_bits_size(const uint32_t header_bits) noexcept;

template
void huffman_table_t<compression_algorithm_e::huffman_only>::set_deflate_header_bits_size(const uint32_t header_bits) noexcept;

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_sw_compression_huffman_table_ptr() const noexcept {
    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    return reinterpret_cast<uint8_t *>(&c_table->sw_compression_table_data);
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_sw_compression_huffman_table_ptr() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_sw_compression_huffman_table_ptr() const noexcept;

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_isal_compression_huffman_table_ptr() const noexcept {
    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    return reinterpret_cast<uint8_t *>(&c_table->isal_compression_table_data);
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_isal_compression_huffman_table_ptr() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_isal_compression_huffman_table_ptr() const noexcept;

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_hw_compression_huffman_table_ptr() const noexcept {
    auto c_table = reinterpret_cast<qpl_compression_huffman_table*>(m_c_huffman_table);

    return reinterpret_cast<uint8_t *>(&c_table->hw_compression_table_data);
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_hw_compression_huffman_table_ptr() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_hw_compression_huffman_table_ptr() const noexcept;

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_sw_decompression_table_buffer() const noexcept {
    auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

    return reinterpret_cast<uint8_t *>(&d_table->sw_flattened_table);
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_sw_decompression_table_buffer() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_sw_decompression_table_buffer() const noexcept;

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_hw_decompression_table_buffer() const noexcept {
    auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

    return reinterpret_cast<uint8_t *>(&d_table->hw_decompression_state);
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_hw_decompression_table_buffer() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_hw_decompression_table_buffer() const noexcept;

template <compression_algorithm_e algorithm>
uint8_t *huffman_table_t<algorithm>::get_deflate_header_buffer() const noexcept {
    auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);

    return reinterpret_cast<uint8_t *>(&d_table->deflate_header_buffer);
}

template
uint8_t *huffman_table_t<compression_algorithm_e::deflate>::get_deflate_header_buffer() const noexcept;

template
uint8_t *huffman_table_t<compression_algorithm_e::huffman_only>::get_deflate_header_buffer() const noexcept;


template <compression_algorithm_e algorithm>
template <execution_path_t execution_path>
bool huffman_table_t<algorithm>::is_representation_used() const noexcept {
    if (execution_path == this->m_meta.path) {
        return true;
    } else {
        return false;
    }
}

template
bool huffman_table_t<compression_algorithm_e::deflate>::is_representation_used<execution_path_t::hardware>() const noexcept;

template
bool huffman_table_t<compression_algorithm_e::deflate>::is_representation_used<execution_path_t::software>() const noexcept;

template
bool huffman_table_t<compression_algorithm_e::huffman_only>::is_representation_used<execution_path_t::hardware>() const noexcept;

template
bool huffman_table_t<compression_algorithm_e::huffman_only>::is_representation_used<execution_path_t::software>() const noexcept;

template <compression_algorithm_e algorithm>
bool huffman_table_t<algorithm>::is_deflate_representation_used() const noexcept {
    auto d_table = reinterpret_cast<qpl_decompression_huffman_table*>(m_d_huffman_table);
    return d_table->representation_mask & QPL_DEFLATE_REPRESENTATION ? true : false;
}

template <compression_algorithm_e algorithm>
allocator_t huffman_table_t<algorithm>::get_internal_allocator() noexcept {
    return details::get_allocator(m_allocator);
}

template
allocator_t huffman_table_t<compression_algorithm_e::deflate>::get_internal_allocator() noexcept;

template
allocator_t huffman_table_t<compression_algorithm_e::huffman_only>::get_internal_allocator() noexcept;

}
