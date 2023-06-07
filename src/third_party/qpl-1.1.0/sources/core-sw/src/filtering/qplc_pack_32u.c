/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for vector packing dword integers to 17...32-bit integers
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_pack_32u17u
 *          - @ref qplc_pack_32u18u
 *          - @ref qplc_pack_32u19u
 *          - @ref qplc_pack_32u20u
 *          - @ref qplc_pack_32u21u
 *          - @ref qplc_pack_32u22u
 *          - @ref qplc_pack_32u23u
 *          - @ref qplc_pack_32u24u
 *          - @ref qplc_pack_32u25u
 *          - @ref qplc_pack_32u26u
 *          - @ref qplc_pack_32u27u
 *          - @ref qplc_pack_32u28u
 *          - @ref qplc_pack_32u29u
 *          - @ref qplc_pack_32u30u
 *          - @ref qplc_pack_32u31u
 *          - @ref qplc_pack_32u32u
 */
#include "own_qplc_defs.h"
#include "qplc_memop.h"

#if PLATFORM >= K0
#include "opt/qplc_pack_32u_k0.h"
#else

OWN_QPLC_INLINE(void, qplc_pack_32u_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t bit_width,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    if (num_elements > 1) {
        int32_t  bits_in_buf  = (int32_t) (bit_width + start_bit);
        uint32_t *src_32u_ptr = (uint32_t *) src_ptr;
        uint32_t *dst_32u_ptr = (uint32_t *) dst_ptr;
        uint64_t src          = (uint64_t) (*dst_32u_ptr) & OWN_BIT_MASK(start_bit);

        src |= ((uint64_t) (*src_32u_ptr)) << start_bit;
        src_32u_ptr++;
        num_elements--;

        while (0u < num_elements) {
            if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
                *dst_32u_ptr = (uint32_t) (src);
                dst_32u_ptr++;
                src = src >> OWN_DWORD_WIDTH;
                bits_in_buf -= OWN_DWORD_WIDTH;
            }
            src = src | (((uint64_t) (*src_32u_ptr)) << bits_in_buf);
            src_32u_ptr++;
            num_elements--;
            bits_in_buf += (int32_t) bit_width;
        }
        dst_ptr               = (uint8_t *) dst_32u_ptr;
        while (0 < bits_in_buf) {
            *dst_ptr = (uint8_t) (src);
            bits_in_buf -= OWN_BYTE_WIDTH;
            dst_ptr++;
            src >>= OWN_BYTE_WIDTH;
        }
    }
    else {
        // In case when there's only one element to pack
        // output buffer size can be less than 32 bits,
        // the following code performs packing byte by byte
        uint64_t    mask = (uint64_t)((1u << bit_width) - 1) << start_bit;
        uint64_t    source = (uint64_t)(*(uint32_t*)src_ptr) << start_bit;
        uint8_t     mask_8u = (uint8_t)mask;
        uint8_t     source_8u;
        uint8_t     dst_8u;

        while (0u == mask_8u) {
            dst_ptr++;
            mask >>= 8;
            source >>= 8;
            mask_8u = (uint8_t)mask;
        }
        source_8u = (uint8_t)source;
        dst_8u = *dst_ptr & (~mask_8u);
        dst_8u |= source_8u;
        *dst_ptr++ = dst_8u;
        mask >>= 8;
        source >>= 8;
        while (mask) {
            mask_8u = (uint8_t)mask;
            source_8u = (uint8_t)source;
            *dst_ptr++ = source_8u;
            mask >>= 8;
            source >>= 8;
        }
    }
}
#endif

// ********************** 17u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u17u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u17u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 17u, dst_ptr, start_bit);
#endif
}

// ********************** 18u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u18u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u18u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 18u, dst_ptr, start_bit);
#endif
}

// ********************** 19u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u19u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u19u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 19u, dst_ptr, start_bit);
#endif
}

// ********************** 20u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u20u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u20u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 20u, dst_ptr, start_bit);
#endif
}

// ********************** 21u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u21u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u21u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 21u, dst_ptr, start_bit);
#endif
}

// ********************** 22u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u22u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u22u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 22u, dst_ptr, start_bit);
#endif
}

// ********************** 23u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u23u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u23u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 23u, dst_ptr, start_bit);
#endif
}

// ********************** 24u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u24u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u24u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 24u, dst_ptr, start_bit);
#endif
}

// ********************** 25u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u25u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u25u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 25u, dst_ptr, start_bit);
#endif
}

// ********************** 26u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u26u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u26u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 26u, dst_ptr, start_bit);
#endif
}

// ********************** 27u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u27u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u27u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 27u, dst_ptr, start_bit);
#endif
}

// ********************** 28u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u28u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u28u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 28u, dst_ptr, start_bit);
#endif
}

// ********************** 29u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u29u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u29u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 29u, dst_ptr, start_bit);
#endif
}

// ********************** 30u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u30u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u30u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 30u, dst_ptr, start_bit);
#endif
}

// ********************** 31u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u31u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_32u31u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_32u_nu(src_ptr, num_elements, 31u, dst_ptr, start_bit);
#endif
}

// ********************** 32u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, num_elements * sizeof(uint32_t));
}
