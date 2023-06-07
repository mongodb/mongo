/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contain implementation of functions for memset and memcpy
 * @date 06/06/2020
 *
 * @details Function list:
 *          - @ref qplc_set_8u
 *          - @ref qplc_set_16u
 *          - @ref qplc_set_32u
 *          - @ref qplc_copy_8u
 *          - @ref qplc_copy_16u
 *          - @ref qplc_copy_32u
 *          - @ref qplc_move_8u
 *          - @ref qplc_move_16u
 *          - @ref qplc_move_32u
 */

#include "own_qplc_defs.h"

#if PLATFORM >= K0

#include "opt/qplc_memop_k0.h"

#endif

OWN_QPLC_FUN(void, qplc_set_8u, (uint8_t value, uint8_t * dst_ptr, uint32_t length)) {
    for (uint32_t i = 0u; i < length; i++) {
        dst_ptr[i] = value;
    }
}

OWN_QPLC_FUN(void, qplc_set_16u, (uint16_t value, uint16_t * dst_ptr, uint32_t length)) {
    for (uint32_t i = 0u; i < length; i++) {
        dst_ptr[i] = value;
    }
}

OWN_QPLC_FUN(void, qplc_set_32u, (uint32_t value, uint32_t * dst_ptr, uint32_t length)) {
    for (uint32_t i = 0u; i < length; i++) {
        dst_ptr[i] = value;
    }
}

OWN_QPLC_FUN(void, qplc_copy_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_copy_8u)(src_ptr, dst_ptr, length);
#else
    const uint64_t *src_64u_ptr = (uint64_t *)src_ptr;
    uint64_t *dst_64u_ptr = (uint64_t *)dst_ptr;

    uint32_t length_64u = length / sizeof(uint64_t);
    uint32_t tail_start = length_64u * sizeof(uint64_t);

    while (length_64u > 3u) {
        dst_64u_ptr[0] = src_64u_ptr[0];
        dst_64u_ptr[1] = src_64u_ptr[1];
        dst_64u_ptr[2] = src_64u_ptr[2];
        dst_64u_ptr[3] = src_64u_ptr[3];

        dst_64u_ptr += 4u;
        src_64u_ptr += 4u;
        length_64u -= 4u;
    }

    for (uint32_t i = 0u; i < length_64u; ++i) {
        dst_64u_ptr[i] = src_64u_ptr[i];
    }

    for (uint32_t i = tail_start; i < length; ++i) {
        dst_ptr[i] = src_ptr[i];
    }
#endif
}

OWN_QPLC_FUN(void, qplc_copy_16u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length)) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, length * sizeof(uint16_t));
}

OWN_QPLC_FUN(void, qplc_copy_32u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length)) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, length * sizeof(uint32_t));
}

/**
 * @brief Performs memset with all zeroes operation
 *
 * @param[in,out]  dst_ptr  pointer to destination byte buffer
 * @param[in]      len      number of bytes to set
 */
OWN_QPLC_FUN(void, qplc_zero_8u, (uint8_t* dst_ptr, uint32_t length)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_zero_8u)(dst_ptr, length);
#else
    uint32_t length_64u = length / sizeof(uint64_t);

    uint64_t* data_64u_ptr = (uint64_t*)dst_ptr;

    // todo: create pragma macros: unroll WIN/LIN
    while (length_64u >= 4) {
        data_64u_ptr[0] = 0u;
        data_64u_ptr[1] = 0u;
        data_64u_ptr[2] = 0u;
        data_64u_ptr[3] = 0u;

        length_64u -= 4;
        data_64u_ptr += 4;
    }

    // todo: Use masks
    for (uint32_t i = 0; i < length_64u; i++) {
        *data_64u_ptr++ = 0u;
    }

    uint32_t remaining_bytes = length % sizeof(uint64_t);

    dst_ptr = (uint8_t*)data_64u_ptr;

    while (remaining_bytes >= 2) {
        dst_ptr[0] = 0u;
        dst_ptr[1] = 0u;

        remaining_bytes -= 2u;
        dst_ptr += 2;
    }

    if (remaining_bytes) {
        *dst_ptr = 0u;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_move_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length )) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_move_8u)(src_ptr,dst_ptr, length);
#else
    if (OWN_QPLC_UINT_PTR(src_ptr) < OWN_QPLC_UINT_PTR(dst_ptr)) {
        for (uint32_t i = 0u; i < length; i++) {
            dst_ptr[length - 1u - i] = src_ptr[length - 1u - i];
        }
    } else {
        for (uint32_t i = 0u; i < length; i++) {
            dst_ptr[i] = src_ptr[i];
        }
    }
#endif
}

OWN_QPLC_FUN(void, qplc_move_16u, (const uint16_t *src_ptr, uint16_t *dst_ptr, uint32_t length )) {
    CALL_CORE_FUN(qplc_move_8u)((const uint8_t *) src_ptr, (uint8_t *) dst_ptr, length * sizeof(uint16_t));
}

OWN_QPLC_FUN(void, qplc_move_32u, (const uint32_t *src_ptr, uint32_t *dst_ptr, uint32_t length )) {
    CALL_CORE_FUN(qplc_move_8u)((const uint8_t *) src_ptr, (uint8_t *) dst_ptr, length * sizeof(uint32_t));
}
