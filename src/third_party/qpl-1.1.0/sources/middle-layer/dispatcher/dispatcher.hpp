/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef DISPATCHER_HPP_
#define DISPATCHER_HPP_

#include <array>
#include <cstdint>
#include <atomic>

#include "qplc_extract.h"
#include "qplc_select.h"
#include "qplc_unpack.h"
#include "qplc_pack.h"
#include "qplc_scan.h"
#include "qplc_memop.h"
#include "qplc_aggregates.h"
#include "qplc_expand.h"
#include "qplc_checksum.h"

#define OWN_MIN_(a, b) (a < b) ? a : b

#define BITS_2_DATA_TYPE_INDEX(x) (OWN_MIN_((((x) - 1u) >> 3u), 2u))

namespace qpl::ml::dispatcher {
enum arch_t {
    px_arch     = 0,
    avx2_arch   = 1,
    avx512_arch = 2
};

auto detect_platform() -> arch_t;

auto get_unpack_index(const uint32_t flag_be, const uint32_t bit_width) -> uint32_t;

auto get_pack_index(const uint32_t flag_be, const uint32_t out_bit_width, const uint32_t flag_nominal) -> uint32_t;

auto get_unpack_prle_index(const uint32_t bit_width) -> uint32_t;

auto get_scan_index(const uint32_t bit_width, const uint32_t scan_flavor_index) -> uint32_t;

auto get_extract_index(const uint32_t bit_width) -> uint32_t;

auto get_select_index(const uint32_t bit_width) -> uint32_t;

auto get_expand_index(const uint32_t bit_width) -> uint32_t;

auto get_pack_bits_index(const uint32_t flag_be,
                         const uint32_t src_bit_width,
                         const uint32_t out_bit_width) -> uint32_t;

auto get_aggregates_index(const uint32_t src_bit_width) -> uint32_t;

auto get_memory_copy_index(const uint32_t bit_width) -> uint32_t;

using unpack_table_t = std::array<qplc_unpack_bits_t_ptr, 64>;

using pack_index_table_t = std::array<qplc_pack_index_t_ptr, 8>;

using unpack_prle_table_t = std::array<qplc_unpack_prle_t_ptr, 3>;

using scan_i_table_t = std::array<qplc_scan_i_t_ptr, 24>;
using scan_table_t = std::array<qplc_scan_t_ptr, 24>;

using pack_table_t = std::array<qplc_pack_bits_t_ptr, 70>;

using extract_table_t = std::array<qplc_extract_t_ptr, 3>;
using extract_i_table_t = std::array<qplc_extract_i_t_ptr, 3>;

using aggregates_table_t = std::array<qplc_aggregates_t_ptr, 4>;

using select_table_t = std::array<qplc_select_t_ptr, 3>;
using select_i_table_t = std::array<qplc_select_i_t_ptr, 3>;

using expand_table_t = std::array<qplc_expand_t_ptr, 3>;

using memory_copy_table_t = std::array<qplc_copy_t_ptr, 3>;
using zero_table_t = std::array<qplc_zero_t_ptr, 1>;
using move_table_t = std::array<qplc_move_t_ptr, 1>;

using crc64_table_t = std::array<qplc_crc64_t_ptr, 1>;
using xor_checksum_table_t = std::array<qplc_xor_checksum_t_ptr, 1>;

using deflate_table_t = std::array<void*, 3u>;

using deflate_fix_table_t = std::array<void*, 1u>;

using setup_dictionary_table_t = std::array<void*, 1u>;

using aggregates_function_ptr_t = aggregates_table_t::value_type;
using extract_function_ptr_t    = extract_table_t::value_type;
using scan_function_ptr         = scan_table_t::value_type;

class kernels_dispatcher final {
public:
    kernels_dispatcher(const kernels_dispatcher &other) = delete;

    auto operator=(const kernels_dispatcher &other) -> kernels_dispatcher & = delete;

    static auto get_instance() noexcept -> kernels_dispatcher &;

    [[nodiscard]] auto get_unpack_table() const noexcept -> const unpack_table_t &;

    [[nodiscard]] auto get_unpack_prle_table() const noexcept -> const unpack_prle_table_t &;

    [[nodiscard]] auto get_pack_index_table() const noexcept -> const pack_index_table_t &;

    [[nodiscard]] auto get_pack_table() const noexcept -> const pack_table_t &;

    [[nodiscard]] auto get_aggregates_table() const noexcept -> const aggregates_table_t &;

    [[nodiscard]] auto get_scan_i_table() const noexcept -> const scan_i_table_t &;

    [[nodiscard]] auto get_scan_table() const noexcept -> const scan_table_t &;

    [[nodiscard]] auto get_extract_table() const noexcept -> const extract_table_t &;

    [[nodiscard]] auto get_extract_i_table() const noexcept -> const extract_i_table_t &;

    [[nodiscard]] auto get_select_table() const noexcept -> const select_table_t &;

    [[nodiscard]] auto get_select_i_table() const noexcept -> const select_i_table_t &;

    [[nodiscard]] auto get_expand_table() const noexcept -> const expand_table_t &;

    [[nodiscard]] auto get_memory_copy_table() const noexcept -> const memory_copy_table_t &;

    [[nodiscard]] auto get_zero_table() const noexcept -> const zero_table_t &;

    [[nodiscard]] auto get_move_table() const noexcept -> const move_table_t &;

    [[nodiscard]] auto get_crc64_table() const noexcept -> const crc64_table_t &;

    [[nodiscard]] auto get_xor_checksum_table() const noexcept -> const xor_checksum_table_t &;

    [[nodiscard]] auto get_deflate_table() const noexcept -> const deflate_table_t &;

    [[nodiscard]] auto get_deflate_fix_table() const noexcept -> const deflate_fix_table_t &;

    [[nodiscard]] auto get_setup_dictionary_table() const noexcept -> const setup_dictionary_table_t &;

protected:
    kernels_dispatcher() noexcept;

private:
    unpack_table_t                  *unpack_table_ptr_                  = nullptr;
    unpack_prle_table_t             *unpack_prle_table_ptr_             = nullptr;
    pack_index_table_t              *pack_index_table_ptr_              = nullptr;
    pack_table_t                    *pack_table_ptr_                    = nullptr;
    scan_i_table_t                  *scan_i_table_ptr_                  = nullptr;
    scan_table_t                    *scan_table_ptr_                    = nullptr;
    extract_table_t                 *extract_table_ptr_                 = nullptr;
    extract_i_table_t               *extract_i_table_ptr_               = nullptr;
    aggregates_table_t              *aggregates_table_ptr_              = nullptr;
    select_table_t                  *select_table_ptr_                  = nullptr;
    select_i_table_t                *select_i_table_ptr_                = nullptr;
    expand_table_t                  *expand_table_ptr_                  = nullptr;
    memory_copy_table_t             *memory_copy_table_ptr_             = nullptr;
    zero_table_t                    *zero_table_ptr_                    = nullptr;
    move_table_t                    *move_table_ptr_                    = nullptr;
    crc64_table_t                   *crc64_table_ptr_                   = nullptr;
    xor_checksum_table_t            *xor_checksum_table_ptr_            = nullptr;
    deflate_table_t                 *deflate_table_ptr_                 = nullptr;
    deflate_fix_table_t             *deflate_fix_table_ptr_             = nullptr;
    setup_dictionary_table_t        *setup_dictionary_table_ptr_        = nullptr;

    arch_t arch_;
};

} // namespace qpl::ml::dispatcher

#endif // DISPATCHER_HPP_
