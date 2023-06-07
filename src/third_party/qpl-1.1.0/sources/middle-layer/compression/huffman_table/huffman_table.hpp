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
#include "common/defs.hpp"
#include "compression/compression_defs.hpp"
#include "qpl/c_api/statistics.h"
#include "qpl/c_api/huffman_table.h"
#include "huffman_table_utils.hpp"

#ifndef QPL_HUFFMAN_TABLE_HPP_
#define QPL_HUFFMAN_TABLE_HPP_

namespace qpl::ml::compression {

enum class huffman_table_type_e : uint8_t {
    combined      = 0u,
    compression   = 1u,
    decompression = 2u,
};

enum class table_version_e : uint8_t {
    v_beta = 0u,
};

constexpr auto LAST_VERSION = table_version_e::v_beta;

/*
    note: struct_id should be bumped to a new id once meta internals have changed
    note: table_version should be bumped to a new version once huffman_table or
          its internals have changed
    this way we could support multiple versions for deserialization
*/
struct huffman_table_meta_t {
    char magic_num[4] = "qpl";
    uint32_t struct_id = 0xAA;
    compression_algorithm_e algorithm;
    table_version_e version;
    huffman_table_type_e type;
    execution_path_t path;
    uint32_t   flags;
};

template<compression_algorithm_e algorithm>
class huffman_table_t {
public:
    huffman_table_t()
        : m_meta()
        , m_is_initialized(false)
        , m_c_huffman_table(nullptr)
        , m_d_huffman_table(nullptr)
        , m_tables_buffer(nullptr, {})
        , m_allocator({})
    {}

    [[nodiscard]] qpl_ml_status create(huffman_table_type_e type, execution_path_t path, allocator_t allocator);

    [[nodiscard]] qpl_ml_status init(const qpl_histogram &histogram_ptr) noexcept;
    [[nodiscard]] qpl_ml_status init(const qpl_triplet *triplet_ptr, const size_t count) noexcept;
    [[nodiscard]] qpl_ml_status init(const huffman_table_t<algorithm> &other) noexcept;

    [[nodiscard]] qpl_ml_status init_with_stream(const uint8_t *const buffer) noexcept;
    [[nodiscard]] qpl_ml_status write_to_stream(uint8_t *const buffer) const noexcept;

    [[nodiscard]] bool is_equal(const huffman_table_t<algorithm> &other) const noexcept;

    [[nodiscard]] bool is_initialized() const noexcept;

    template <execution_path_t execution_path>
    [[nodiscard]] bool is_representation_used() const noexcept;

    template <execution_path_t execution_path>
    [[nodiscard]] uint8_t* compression_huffman_table() const noexcept;

    template <execution_path_t execution_path>
    [[nodiscard]] uint8_t* decompression_huffman_table() const noexcept;

    [[nodiscard]] uint8_t *get_lookup_table_buffer_ptr() const noexcept;

    [[nodiscard]] void *get_aecs_decompress() const noexcept;

    [[nodiscard]] uint32_t *get_literals_lengths_table_ptr() const noexcept;

    [[nodiscard]] uint32_t *get_offsets_table_ptr() const noexcept;

    [[nodiscard]] uint8_t *get_deflate_header_ptr() const noexcept;

    [[nodiscard]] uint32_t get_deflate_header_bits_size() const noexcept;

    void set_deflate_header_bits_size(uint32_t header_bits) noexcept;

    [[nodiscard]] uint8_t *get_sw_compression_huffman_table_ptr() const noexcept;

    [[nodiscard]] uint8_t *get_isal_compression_huffman_table_ptr() const noexcept;

    [[nodiscard]] uint8_t *get_hw_compression_huffman_table_ptr() const noexcept;

    [[nodiscard]] uint8_t *get_sw_decompression_table_buffer() const noexcept;

    [[nodiscard]] uint8_t *get_hw_decompression_table_buffer() const noexcept;

    [[nodiscard]] uint8_t *get_deflate_header_buffer() const noexcept;

    [[nodiscard]] bool is_deflate_representation_used() const noexcept;

    [[nodiscard]] allocator_t get_internal_allocator() noexcept;

private:
    huffman_table_meta_t       m_meta{};
    bool                       m_is_initialized{};
    uint8_t *                  m_c_huffman_table{};
    uint8_t *                  m_d_huffman_table{};
    std::unique_ptr<uint8_t[], void(*)(void*)> m_tables_buffer{nullptr, {}};
    allocator_t                m_allocator{};
};

}

#endif //QPL_HUFFMAN_TABLE_HPP_
