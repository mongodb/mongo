/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "dispatcher.hpp"

#ifdef _WIN32

#include "intrin.h"
//  Windows CPUID
#define cpuid(info, x)    __cpuidex(info, x, 0)
#else
//  GCC Intrinsics
#include <cpuid.h>
#include <dlfcn.h>

void cpuid(int info[4], int InfoType) {
    __cpuid_count(InfoType, 0, info[0], info[1], info[2], info[3]);
}

unsigned long long _xgetbv(unsigned int index) {
    unsigned int eax, edx;
    __asm__ __volatile__(
    "xgetbv;"
    : "=a" (eax), "=d"(edx)
    : "c" (index)
    );
    return ((unsigned long long) edx << 32) | eax;
}

#endif

#define CPUID_AVX512F       0x00010000
#define CPUID_AVX512CD      0x10000000
#define CPUID_AVX512VL      0x80000000
#define CPUID_AVX512BW      0x40000000
#define CPUID_AVX512DQ      0x00020000
#define EXC_OSXSAVE         0x08000000 // 27th  bit

// CPUID_AVX512_MASK covers all the instructions used in middle-layer.
// Intel® Intelligent Storage Acceleration Library (Intel® ISA-L) component has
// a standalone dispatching logic and has its own masks.
#define CPUID_AVX512_MASK (CPUID_AVX512F | CPUID_AVX512CD | CPUID_AVX512VL | CPUID_AVX512BW | CPUID_AVX512DQ)

namespace qpl::ml::dispatcher {
class kernel_dispatcher_singleton
{
public:
    kernel_dispatcher_singleton()
    {
        (void)kernels_dispatcher::get_instance();
    }
};
static kernel_dispatcher_singleton g_kernel_dispatcher_singleton;

extern unpack_table_t px_unpack_table;
extern unpack_table_t avx512_unpack_table;

extern pack_index_table_t px_pack_index_table;
extern pack_index_table_t avx512_pack_index_table;

extern unpack_prle_table_t px_unpack_prle_table;
extern unpack_prle_table_t avx512_unpack_prle_table;

extern scan_i_table_t px_scan_i_table;
extern scan_i_table_t avx512_scan_i_table;

extern scan_table_t px_scan_table;
extern scan_table_t avx512_scan_table;

extern pack_table_t px_pack_table;
extern pack_table_t avx512_pack_table;

extern extract_table_t px_extract_table;
extern extract_table_t avx512_extract_table;

extern extract_i_table_t px_extract_i_table;
extern extract_i_table_t avx512_extract_i_table;

extern aggregates_table_t px_aggregates_table;
extern aggregates_table_t avx512_aggregates_table;

extern select_table_t px_select_table;
extern select_table_t avx512_select_table;

extern select_i_table_t px_select_i_table;
extern select_i_table_t avx512_select_i_table;

extern expand_table_t px_expand_table;
extern expand_table_t avx512_expand_table;

extern memory_copy_table_t px_memory_copy_table;
extern memory_copy_table_t avx512_memory_copy_table;

extern zero_table_t px_zero_table;
extern zero_table_t avx512_zero_table;

extern move_table_t px_move_table;
extern move_table_t avx512_move_table;

extern crc64_table_t px_crc64_table;
extern crc64_table_t avx512_crc64_table;

extern xor_checksum_table_t px_xor_checksum_table;
extern xor_checksum_table_t avx512_xor_checksum_table;

extern deflate_table_t px_deflate_table;
extern deflate_table_t avx512_deflate_table;

extern deflate_fix_table_t px_deflate_fix_table;
extern deflate_fix_table_t avx512_deflate_fix_table;

extern setup_dictionary_table_t px_setup_dictionary_table;
extern setup_dictionary_table_t avx512_setup_dictionary_table;


auto detect_platform() -> arch_t {
    arch_t detected_platform = arch_t::px_arch;
    int    cpu_info[4];
    cpuid(cpu_info, 7);
    bool avx512_support_cpu  = ((cpu_info[1] & CPUID_AVX512_MASK) == CPUID_AVX512_MASK);

    cpuid(cpu_info, 1);
    bool os_uses_XSAVE_XSTORE = cpu_info[2] & EXC_OSXSAVE;

    // Check if XGETBV enabled for application use
    if (os_uses_XSAVE_XSTORE) {
        unsigned long long xcr_feature_mask = _xgetbv(0);
        // Check if OPMASK state and ZMM state are enabled
        if ((xcr_feature_mask & 0xe0) == 0xe0) {
             // Check if XMM state and YMM state are enabled
             if ((xcr_feature_mask & 0x6) == 0x6) {
                // Check if AVX512 features are supported
                if (avx512_support_cpu) {
                    detected_platform = arch_t::avx512_arch;
                }
            }
        }
    }

    return detected_platform;
}

auto get_unpack_index(const uint32_t flag_be, const uint32_t bit_width) -> uint32_t {
    uint32_t input_be_shift = (flag_be) ? 32u : 0u;
    // Unpack function table contains 64 entries - starts from 1-32 bit-width for le_format, then 1-32 for BE input
    uint32_t unpack_index   = input_be_shift + bit_width - 1u;

    return unpack_index;
}

auto get_pack_index(const uint32_t flag_be, const uint32_t out_bit_width, const uint32_t flag_nominal) -> uint32_t {
    uint32_t output_be_shift = (flag_be) ? 4u : 0u;
    // Pack function table for nominal bit-vector output contains 8 entries - starts from 0-3 qpl_out_format for le_format,
    // then 4-7 for BE output
    uint32_t pack_index      = (flag_nominal) ? out_bit_width + output_be_shift : output_be_shift;

    return pack_index;
}

auto get_unpack_prle_index(const uint32_t bit_width) -> uint32_t {
    return BITS_2_DATA_TYPE_INDEX(bit_width);
}

auto kernels_dispatcher::get_instance() noexcept -> kernels_dispatcher & {
    static kernels_dispatcher instance{};

    return instance;
}

auto get_scan_index(const uint32_t bit_width, const uint32_t scan_flavor_index) -> uint32_t {
    // Scan function table contains 3 entries for each scan sub-operation - for 8u, 16u & 32u unpacked data;
    uint32_t data_type_index = BITS_2_DATA_TYPE_INDEX(bit_width);
    // Shift scan function index to the corresponding scan sub op: EQ, NE,LT, le_format, GT, GE, RANGE, NOT_RANGE
    uint32_t scan_index      = data_type_index + scan_flavor_index * 3u;

    return scan_index;
}

auto get_extract_index(const uint32_t bit_width) -> uint32_t {
    // Extract function table contains 3 entries for 8u, 16u & 32u unpacked data;
    uint32_t extract_index = BITS_2_DATA_TYPE_INDEX(bit_width);

    return extract_index;
}

auto get_select_index(const uint32_t bit_width) -> uint32_t {
    // Select function table contains 3 entries for 8u, 16u & 32u unpacked data;
    uint32_t select_index = BITS_2_DATA_TYPE_INDEX(bit_width);

    return select_index;
}

auto get_expand_index(const uint32_t bit_width) -> uint32_t {
    // Expand function table contains 3 entries for 8u, 16u & 32u unpacked data;
    uint32_t expand_index = BITS_2_DATA_TYPE_INDEX(bit_width);

    return expand_index;
}

auto get_memory_copy_index(const uint32_t bit_width) -> uint32_t {
    // Memory copy function table contains 3 entries for 8u, 16u & 32u unpacked data;
    uint32_t memory_copy_index = BITS_2_DATA_TYPE_INDEX(bit_width);

    return memory_copy_index;
}

auto get_pack_bits_index(const uint32_t flag_be,
                         const uint32_t src_bit_width,
                         const uint32_t out_bit_width) -> uint32_t {
    uint32_t pack_array_index = src_bit_width - 1u;
    uint32_t input_be_shift   = (flag_be) ? 35 : 0u; // 35
    // Unpack function table contains 70 (2 * 35) entries - starts from 1-32 bit-width
    // for le_format + 8u16u|8u32u|16u32u cases, then the same for BE input
    if (out_bit_width) {
        // Apply output modification for nominal array output
        if (8u >= src_bit_width) {
            switch (out_bit_width) {
                case 1u: {
                    pack_array_index = 7u; /**< 8u->8u */
                    break;
                }
                case 2u: {
                    pack_array_index = 32u; /**< 8u->16u */
                    break;
                }
                case 3u: {
                    pack_array_index = 33u; /**< 8u->32u */
                    break;
                }
                default: {
                    break;
                }
            }
        } else if (16u >= src_bit_width) {
            switch (out_bit_width) {
                case 1u: {
                    break;
                }
                case 2u: {
                    pack_array_index = 15u; /**< 16u->16u */
                    break;
                }
                case 3u: {
                    pack_array_index = 34u; /**< 16u->32u */
                    break;
                }
                default: {
                    break;
                }
            }
        } else {    /**< 32u >= src_bit_width */
            pack_array_index = 31u; /**< 32u->32u */
        }
    }
    // No output modification for nominal array output
    pack_array_index          = input_be_shift + pack_array_index;

    return pack_array_index;
}

auto get_aggregates_index(const uint32_t src_bit_width) -> uint32_t {
    uint32_t aggregates_index = BITS_2_DATA_TYPE_INDEX(src_bit_width);
    aggregates_index = (1u == src_bit_width) ? 0u : aggregates_index + 1u;

    return aggregates_index;
}

auto kernels_dispatcher::get_unpack_table() const noexcept -> const unpack_table_t & {
    return *unpack_table_ptr_;
}

auto kernels_dispatcher::get_unpack_prle_table() const noexcept -> const unpack_prle_table_t & {
    return *unpack_prle_table_ptr_;
}

auto kernels_dispatcher::get_pack_index_table() const noexcept -> const pack_index_table_t & {
    return *pack_index_table_ptr_;
}

auto kernels_dispatcher::get_pack_table() const noexcept -> const pack_table_t & {
    return *pack_table_ptr_;
}

auto kernels_dispatcher::get_scan_i_table() const noexcept -> const scan_i_table_t & {
    return *scan_i_table_ptr_;
}

auto kernels_dispatcher::get_scan_table() const noexcept -> const scan_table_t & {
    return *scan_table_ptr_;
}

auto kernels_dispatcher::get_aggregates_table() const noexcept -> const aggregates_table_t & {
    return *aggregates_table_ptr_;
}

auto kernels_dispatcher::get_extract_table() const noexcept -> const extract_table_t & {
    return *extract_table_ptr_;
}

auto kernels_dispatcher::get_extract_i_table() const noexcept -> const extract_i_table_t & {
    return *extract_i_table_ptr_;
}

auto kernels_dispatcher::get_select_table() const noexcept -> const select_table_t & {
    return *select_table_ptr_;
}

auto kernels_dispatcher::get_select_i_table() const noexcept -> const select_i_table_t & {
    return *select_i_table_ptr_;
}

auto kernels_dispatcher::get_memory_copy_table() const noexcept -> const memory_copy_table_t & {
    return *memory_copy_table_ptr_;
}

auto kernels_dispatcher::get_zero_table() const noexcept -> const zero_table_t & {
    return *zero_table_ptr_;
}

auto kernels_dispatcher::get_move_table() const noexcept -> const move_table_t& {
    return *move_table_ptr_;
}

auto kernels_dispatcher::get_crc64_table() const noexcept -> const crc64_table_t &{
    return *crc64_table_ptr_;
}

auto kernels_dispatcher::get_xor_checksum_table() const noexcept -> const xor_checksum_table_t & {
    return *xor_checksum_table_ptr_;
}

auto kernels_dispatcher::get_deflate_table() const noexcept -> const deflate_table_t & {
    return *deflate_table_ptr_;
}

auto kernels_dispatcher::get_setup_dictionary_table() const noexcept -> const setup_dictionary_table_t & {
    return *setup_dictionary_table_ptr_;
}

auto kernels_dispatcher::get_deflate_fix_table() const noexcept -> const deflate_fix_table_t & {
    return *deflate_fix_table_ptr_;
}


auto kernels_dispatcher::get_expand_table() const noexcept -> const expand_table_t & {
    return *expand_table_ptr_;
}

kernels_dispatcher::kernels_dispatcher() noexcept {
    arch_ = detect_platform();

    switch (arch_) {
        case arch_t::avx512_arch: {
            unpack_table_ptr_                = &avx512_unpack_table;
            // This is a bug, should be fixed, avx512_prle kernel fails the tests
            unpack_prle_table_ptr_           = &px_unpack_prle_table;
            pack_index_table_ptr_            = &avx512_pack_index_table;
            pack_table_ptr_                  = &avx512_pack_table;
            scan_i_table_ptr_                = &avx512_scan_i_table;
            scan_table_ptr_                  = &avx512_scan_table;
            extract_table_ptr_               = &avx512_extract_table;
            extract_i_table_ptr_             = &avx512_extract_i_table;
            aggregates_table_ptr_            = &avx512_aggregates_table;
            select_table_ptr_                = &avx512_select_table;
            select_i_table_ptr_              = &avx512_select_i_table;
            expand_table_ptr_                = &avx512_expand_table;
            memory_copy_table_ptr_           = &avx512_memory_copy_table;
            zero_table_ptr_                  = &avx512_zero_table;
            move_table_ptr_                  = &avx512_move_table;
            crc64_table_ptr_                 = &avx512_crc64_table;
            xor_checksum_table_ptr_          = &avx512_xor_checksum_table;
            deflate_table_ptr_               = &avx512_deflate_table;
            deflate_fix_table_ptr_           = &avx512_deflate_fix_table;
            setup_dictionary_table_ptr_      = &avx512_setup_dictionary_table;
            break;
        }
        default: {
            unpack_table_ptr_                = &px_unpack_table;
            unpack_prle_table_ptr_           = &px_unpack_prle_table;
            pack_index_table_ptr_            = &px_pack_index_table;
            pack_table_ptr_                  = &px_pack_table;
            scan_i_table_ptr_                = &px_scan_i_table;
            scan_table_ptr_                  = &px_scan_table;
            extract_table_ptr_               = &px_extract_table;
            extract_i_table_ptr_             = &px_extract_i_table;
            aggregates_table_ptr_            = &px_aggregates_table;
            select_table_ptr_                = &px_select_table;
            select_i_table_ptr_              = &px_select_i_table;
            expand_table_ptr_                = &px_expand_table;
            memory_copy_table_ptr_           = &px_memory_copy_table;
            zero_table_ptr_                  = &px_zero_table;
            move_table_ptr_                  = &px_move_table;
            crc64_table_ptr_                 = &px_crc64_table;
            xor_checksum_table_ptr_          = &px_xor_checksum_table;
            deflate_table_ptr_               = &px_deflate_table;
            deflate_fix_table_ptr_           = &px_deflate_fix_table;
            setup_dictionary_table_ptr_      = &px_setup_dictionary_table;
        }
    }
}

} // namespace ml::dispatcher
