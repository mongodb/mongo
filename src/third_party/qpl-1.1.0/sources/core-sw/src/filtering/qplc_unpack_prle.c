/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for PRLE fromat unpacking to bytes, words & dwords
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_unpack_prle_8u
 *          - @ref qplc_unpack_prle_16u
 *          - @ref qplc_unpack_prle_32u
 *
 */

#include "own_qplc_defs.h"
#include "qplc_api.h"
#include "qplc_memop.h"

OWN_QPLC_INLINE(qplc_status_t, ownc_decode_prle_header, (uint8_t * *pp_src,
        const uint8_t *src_stop_ptr,
        uint32_t      *format_ptr,
        uint32_t      *count_ptr)) {
    OWN_RETURN_ERROR((*pp_src >= src_stop_ptr), QPLC_STS_SRC_IS_SHORT_ERR);
    uint32_t value;

    value = (uint32_t) (*(*pp_src)++);
    *format_ptr = value & OWN_LOW_BIT_MASK;
    *count_ptr  = OWN_PRLE_COUNT(value);
    if (value & OWN_HIGH_BIT_MASK) {
        OWN_RETURN_ERROR((*pp_src >= src_stop_ptr), QPLC_STS_SRC_IS_SHORT_ERR);
        value = (uint32_t) (*(*pp_src)++);
        *count_ptr |= OWN_VARINT_BYTE_1(value);
        if (value & OWN_HIGH_BIT_MASK) {
            OWN_RETURN_ERROR((*pp_src >= src_stop_ptr), QPLC_STS_SRC_IS_SHORT_ERR);
            value = (uint32_t) (*(*pp_src)++);
            *count_ptr |= OWN_VARINT_BYTE_2(value);
            if (value & OWN_HIGH_BIT_MASK) {
                OWN_RETURN_ERROR((*pp_src >= src_stop_ptr), QPLC_STS_SRC_IS_SHORT_ERR);
                value = (uint32_t) (*(*pp_src)++);
                *count_ptr |= OWN_VARINT_BYTE_3(value);
                if (value & OWN_HIGH_BIT_MASK) {
                    OWN_RETURN_ERROR((*pp_src >= src_stop_ptr), QPLC_STS_SRC_IS_SHORT_ERR);
                    value = (uint32_t) (*(*pp_src)++);
                    *count_ptr |= OWN_VARINT_BYTE_4(value);
                }
            }
        }
    }
    return QPLC_STS_OK;
}

OWN_QPLC_INLINE(uint32_t, ownc_octa_part_8u, (uint8_t * *pp_src,
        const uint8_t *src_stop_ptr,
        uint8_t       **pp_dst,
        const uint8_t *dst_stop_ptr)) {
    uint32_t max_count_src = (uint32_t) (src_stop_ptr - *pp_src);
    uint32_t max_count_dst = (uint32_t) (dst_stop_ptr - *pp_dst);
    uint32_t max_count     = QPL_MIN(max_count_src, max_count_dst);
    if (0u == max_count) {
        return 0u;
    } else {
        CALL_CORE_FUN(qplc_copy_8u)(*pp_src, *pp_dst, max_count);
        *pp_src += max_count;
        *pp_dst += max_count;
        return (OWN_PARQUET_WIDTH - max_count);
    }
}

OWN_QPLC_INLINE(uint32_t, ownc_octa_part_16u, (uint8_t * *pp_src,
        const uint8_t *src_stop_ptr,
        uint8_t       **pp_dst,
        const uint8_t *dst_stop_ptr)) {
    uint32_t max_count_src = (uint32_t) (src_stop_ptr - *pp_src) / sizeof(uint16_t);
    uint32_t max_count_dst = (uint32_t) (dst_stop_ptr - *pp_dst) / sizeof(uint16_t);
    uint32_t max_count     = QPL_MIN(max_count_src, max_count_dst);
    if (0u == max_count) {
        return 0u;
    } else {
        CALL_CORE_FUN(qplc_copy_16u)(*pp_src, *pp_dst, max_count);
        *pp_src += max_count * sizeof(uint16_t);
        *pp_dst += max_count * sizeof(uint16_t);
        return (OWN_PARQUET_WIDTH - max_count);
    }
}

OWN_QPLC_INLINE(uint32_t, ownc_octa_part_32u, (uint8_t * *pp_src,
        const uint8_t *src_stop_ptr,
        uint8_t       **pp_dst,
        const uint8_t *dst_stop_ptr)) {
    uint32_t max_count_src = (uint32_t) (src_stop_ptr - *pp_src) / sizeof(uint32_t);
    uint32_t max_count_dst = (uint32_t) (dst_stop_ptr - *pp_dst) / sizeof(uint32_t);
    uint32_t max_count     = QPL_MIN(max_count_src, max_count_dst);
    if (0u == max_count) {
        return 0u;
    } else {
        CALL_CORE_FUN(qplc_copy_32u)(*pp_src, *pp_dst, max_count);
        *pp_src += max_count * sizeof(uint32_t);
        *pp_dst += max_count * sizeof(uint32_t);
        return (OWN_PARQUET_WIDTH - max_count);
    }
}

OWN_QPLC_FUN(qplc_status_t, qplc_unpack_prle_8u, (uint8_t * *pp_src,
        uint32_t src_length,
        uint32_t bit_width,
        uint8_t * *pp_dst,
        uint32_t dst_length,
        int32_t * count_ptr,
        uint32_t * value_ptr)) {
    uint32_t count;
    uint32_t format;
    uint8_t  value;
    uint32_t max_count;
    uint32_t max_count_src;
    uint32_t max_count_dst;
    uint32_t src_step;
    uint32_t dst_step;
    uint8_t  *kept_src_ptr;
    uint8_t  *dst_ptr      = (uint8_t *) *pp_dst;
    uint8_t  *src_ptr      = (uint8_t *) *pp_src;
    uint8_t  *src_stop_ptr = src_ptr + src_length;
    uint8_t  *dst_stop_ptr = dst_ptr + dst_length;
    uint32_t status        = QPLC_STS_OK;

    if (0 < *count_ptr) {
        count = OWN_MIN((uint32_t) *count_ptr, dst_length);
        *count_ptr = *count_ptr - (int32_t) count;
        value  = (uint8_t) *value_ptr;
        CALL_CORE_FUN(qplc_set_8u)(value, dst_ptr, count);
        dst_ptr += count;
        status = (0 != *count_ptr) ? QPLC_STS_DST_IS_SHORT_ERR : status;
    }
    if (0 > *count_ptr) {
        max_count     = (uint32_t) (-*count_ptr);
        max_count_dst = dst_length / OWN_PARQUET_WIDTH;
        max_count_src = src_length / bit_width;
        count         = OWN_MIN(max_count_src, max_count_dst);
        status        = (count == max_count_src) ? QPLC_STS_SRC_IS_SHORT_ERR : QPLC_STS_DST_IS_SHORT_ERR;
        count         = OWN_MIN(max_count, count);
        *count_ptr = *count_ptr + (int32_t) count;
        (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
        dst_ptr += count * OWN_PARQUET_WIDTH;
        src_ptr += count * bit_width;
        if (0 != *count_ptr) {
            if (8u == bit_width) {
                *value_ptr = ownc_octa_part_8u(&src_ptr, src_stop_ptr, &dst_ptr, dst_stop_ptr);
            }
            else {
                uint32_t max_count_lit_src = (uint32_t) (src_stop_ptr - src_ptr) / bit_width * OWN_PARQUET_WIDTH;
                uint32_t max_count_lit_dst = (uint32_t) (dst_stop_ptr - dst_ptr);
                uint32_t max_count_lit = QPL_MIN(max_count_lit_src, max_count_lit_dst);
                (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, max_count_lit, 0u, dst_ptr);
                src_ptr += max_count_lit * (bit_width / OWN_PARQUET_WIDTH);
                dst_ptr += max_count_lit;
            }
            *pp_dst = dst_ptr;
            *pp_src = src_ptr;
            return status;
        }
        status = QPLC_STS_OK;
    }
    while ((src_ptr < src_stop_ptr) && (dst_ptr < dst_stop_ptr)) {
        kept_src_ptr = src_ptr;
        // Extract format and counter
        status       = ownc_decode_prle_header(&src_ptr, src_stop_ptr, &format, &count);
        if (status != QPLC_STS_OK) {
            src_ptr = kept_src_ptr;
            break;
        }
        if (OWN_LITERAL_OCTA_GROUP == format) {
            // This is a set of qplc_packed bit_width-integers (octa-groups)
            src_step = count * bit_width;
            dst_step = count * OWN_PARQUET_WIDTH;
            if (((src_ptr + src_step) > src_stop_ptr) || ((dst_ptr + dst_step) > dst_stop_ptr)) {
                max_count_src = (uint32_t) (src_stop_ptr - src_ptr) / bit_width;
                max_count_dst = (uint32_t) ((dst_stop_ptr - dst_ptr) / OWN_PARQUET_WIDTH);
                max_count     = QPL_MIN(max_count_src, max_count_dst);
                status        = (max_count == max_count_src) ? QPLC_STS_SRC_IS_SHORT_ERR : QPLC_STS_DST_IS_SHORT_ERR;
                *count_ptr = -(int32_t) (count - max_count);
                count = max_count;
                (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
                src_ptr += count * bit_width;
                dst_ptr += count * OWN_PARQUET_WIDTH;
                break;
            }

            if (count > 0) {
                (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
            }

            src_ptr += src_step;
            dst_ptr += dst_step;
        } else {
            // This is a set of RLE-qplc_packed bit_width-integers
            if ((src_ptr + sizeof(uint8_t)) > src_stop_ptr) {
                status  = QPLC_STS_SRC_IS_SHORT_ERR;
                src_ptr = kept_src_ptr;
                break;
            }
            value = *src_ptr++;
            if ((dst_ptr + count) > dst_stop_ptr) {
                max_count = (uint32_t) (dst_stop_ptr - dst_ptr);
                *count_ptr = (int32_t) (count - max_count);
                count = max_count;
                *value_ptr = value;
                status = QPLC_STS_DST_IS_SHORT_ERR;
            }
            CALL_CORE_FUN(qplc_set_8u)(value, dst_ptr, count);
            dst_ptr += count;
        }
    }
    *pp_dst = dst_ptr;
    *pp_src = src_ptr;
    return status;
}

OWN_QPLC_FUN(qplc_status_t, qplc_unpack_prle_16u, (uint8_t * *pp_src,
        uint32_t src_length,
        uint32_t bit_width,
        uint8_t * *pp_dst,
        uint32_t dst_length,
        int32_t * count_ptr,
        uint32_t * value_ptr)) {
    uint32_t count;
    uint32_t format;
    uint16_t value;
    uint32_t max_count;
    uint32_t max_count_src;
    uint32_t max_count_dst;
    uint8_t  *dst_ptr      = (uint8_t *) *pp_dst;
    uint8_t  *src_ptr      = (uint8_t *) *pp_src;
    uint8_t  *src_stop_ptr = src_ptr + src_length;
    // dst_length is length in unpacked elements;
    dst_length *= sizeof(uint16_t);
    uint8_t  *dst_stop_ptr = dst_ptr + dst_length;
    uint8_t  *kept_src_ptr;
    uint32_t src_step;
    uint32_t dst_step;
    uint32_t status        = QPLC_STS_OK;

    if (0 < *count_ptr) {
        count = QPL_MIN((uint32_t) *count_ptr, dst_length / sizeof(uint16_t));
        *count_ptr = *count_ptr - (int32_t) count;
        value  = (uint16_t) *value_ptr;
        CALL_CORE_FUN(qplc_set_16u)(value, (uint16_t *) dst_ptr, count);
        dst_ptr += count * sizeof(uint16_t);
        status = (0 != *count_ptr) ? QPLC_STS_DST_IS_SHORT_ERR : status;
    }
    if (0 > *count_ptr) {
        max_count     = (uint32_t) (-*count_ptr);
        max_count_dst = dst_length / (OWN_PARQUET_WIDTH * sizeof(uint16_t));
        max_count_src = src_length / bit_width;
        count         = OWN_MIN(max_count_src, max_count_dst);
        status        = (count == max_count_src) ? QPLC_STS_SRC_IS_SHORT_ERR : QPLC_STS_DST_IS_SHORT_ERR;
        count         = OWN_MIN(max_count, count);
        *count_ptr = *count_ptr + (int32_t) count;
        (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
        dst_ptr += count * OWN_PARQUET_WIDTH * sizeof(uint16_t);
        src_ptr += count * bit_width;
        if (0 != *count_ptr) {
            if (16u == bit_width) {
                *value_ptr = ownc_octa_part_16u(&src_ptr, src_stop_ptr, &dst_ptr, dst_stop_ptr);
            }
            else {
                uint32_t max_count_lit_src = (uint32_t) (src_stop_ptr - src_ptr) / bit_width * OWN_PARQUET_WIDTH;
                uint32_t max_count_lit_dst = (uint32_t) (dst_stop_ptr - dst_ptr) / sizeof(uint16_t);
                uint32_t max_count_lit = QPL_MIN(max_count_lit_src, max_count_lit_dst);
                (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, max_count_lit, 0u, dst_ptr);
                src_ptr += max_count_lit * (bit_width / OWN_PARQUET_WIDTH);
                dst_ptr += max_count_lit * sizeof(uint16_t);
            }
            *pp_dst = dst_ptr;
            *pp_src = src_ptr;
            return status;
        }
        status = QPLC_STS_OK;
    }
    while ((src_ptr < src_stop_ptr) && (dst_ptr < dst_stop_ptr)) {
        kept_src_ptr = src_ptr;
        // Extract format and counter
        status       = ownc_decode_prle_header(&src_ptr, src_stop_ptr, &format, &count);
        if (status != QPLC_STS_OK) {
            src_ptr = kept_src_ptr;
            break;
        }
        if (OWN_LITERAL_OCTA_GROUP == format) {
            // This is a set of qplc_packed bit_width-integers (octa-groups)
            src_step = count * bit_width;
            dst_step = count * OWN_PARQUET_WIDTH * sizeof(uint16_t);
            if (((src_ptr + src_step) > src_stop_ptr) || ((dst_ptr + dst_step) > dst_stop_ptr)) {
                max_count_src = (uint32_t) (src_stop_ptr - src_ptr) / bit_width;
                max_count_dst = (uint32_t) ((dst_stop_ptr - dst_ptr) / (OWN_PARQUET_WIDTH * sizeof(uint16_t)));
                max_count     = QPL_MIN(max_count_src, max_count_dst);
                status        = (max_count == max_count_src) ? QPLC_STS_SRC_IS_SHORT_ERR : QPLC_STS_DST_IS_SHORT_ERR;
                *count_ptr = -(int32_t) (count - max_count);
                count = max_count;
                (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
                src_ptr += count * bit_width;
                dst_ptr += count * OWN_PARQUET_WIDTH * sizeof(uint16_t);
                break;
            }
            (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
            src_ptr += src_step;
            dst_ptr += dst_step;
        } else {
            // This is a set of RLE-qplc_packed bit_width-integers
            if ((src_ptr + sizeof(uint16_t)) > src_stop_ptr) {
                status  = QPLC_STS_SRC_IS_SHORT_ERR;
                src_ptr = kept_src_ptr;
                break;
            }
            value = *(uint16_t *) src_ptr;
            src_ptr += sizeof(uint16_t);
            if ((dst_ptr + count * sizeof(uint16_t)) > dst_stop_ptr) {
                max_count = (uint32_t) ((dst_stop_ptr - dst_ptr) / sizeof(uint16_t));
                *count_ptr = (int32_t) (count - max_count);
                count = max_count;
                *value_ptr = value;
                status = QPLC_STS_DST_IS_SHORT_ERR;
            }
            CALL_CORE_FUN(qplc_set_16u)(value, (uint16_t *) dst_ptr, count);
            dst_ptr += count * sizeof(uint16_t);
        }
    }
    *pp_dst = dst_ptr;
    *pp_src = src_ptr;
    return status;
}

OWN_QPLC_FUN(qplc_status_t, qplc_unpack_prle_32u, (uint8_t * *pp_src,
        uint32_t src_length,
        uint32_t bit_width,
        uint8_t * *pp_dst,
        uint32_t dst_length,
        int32_t * count_ptr,
        uint32_t * value_ptr)) {
    uint32_t count;
    uint32_t format;
    uint32_t value;
    uint32_t max_count;
    uint32_t max_count_src;
    uint32_t max_count_dst;
    uint8_t  *dst_ptr      = (uint8_t *) *pp_dst;
    uint8_t  *src_ptr      = (uint8_t *) *pp_src;
    uint8_t  *src_stop_ptr = src_ptr + src_length;
    // dst_length is length in unpacked elements;
    dst_length *= sizeof(uint32_t);
    uint8_t  *dst_stop_ptr = dst_ptr + dst_length;
    uint8_t  *kept_src_ptr;
    uint32_t src_step;
    uint32_t dst_step;
    uint32_t status        = QPLC_STS_OK;
    // Using a fixed-width of round-up-to-next-byte(bit-width) - value may take 3 or 4 bytes
    uint32_t value_width   = (OWN_3_BYTE_WIDTH < bit_width) ? 4u : 3u;
    uint32_t value_mask    = (OWN_3_BYTE_WIDTH < bit_width) ? UINT32_MAX : UINT32_MAX >> OWN_BYTE_WIDTH;

    if (0 < *count_ptr) {
        count = QPL_MIN((uint32_t) *count_ptr, dst_length / sizeof(uint32_t));
        *count_ptr = *count_ptr - (int32_t) count;
        value  = (uint32_t) *value_ptr;
        CALL_CORE_FUN(qplc_set_32u)(value, (uint32_t *) dst_ptr, count);
        dst_ptr += count * sizeof(uint32_t);
        status = (0 != *count_ptr) ? QPLC_STS_DST_IS_SHORT_ERR : status;
    }
    if (0 > *count_ptr) {
        max_count     = (uint32_t) (-*count_ptr);
        max_count_dst = dst_length / (OWN_PARQUET_WIDTH * sizeof(uint32_t));
        max_count_src = src_length / bit_width;
        count         = OWN_MIN(max_count_src, max_count_dst);
        status        = (count == max_count_src) ? QPLC_STS_SRC_IS_SHORT_ERR : QPLC_STS_DST_IS_SHORT_ERR;
        count         = OWN_MIN(max_count, count);
        *count_ptr = *count_ptr + (int32_t) count;
        (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
        dst_ptr += count * OWN_PARQUET_WIDTH * sizeof(uint32_t);
        src_ptr += count * bit_width;
        if (0 != *count_ptr) {
            if (32u == bit_width) {
                *value_ptr = ownc_octa_part_32u(&src_ptr, src_stop_ptr, &dst_ptr, dst_stop_ptr);
            }
            else {
                uint32_t max_count_lit_src = (uint32_t) (src_stop_ptr - src_ptr) / bit_width * OWN_PARQUET_WIDTH;
                uint32_t max_count_lit_dst = (uint32_t) (dst_stop_ptr - dst_ptr) / sizeof(uint32_t);
                uint32_t max_count_lit = QPL_MIN(max_count_lit_src, max_count_lit_dst);
                (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, max_count_lit, 0u, dst_ptr);
                src_ptr += max_count_lit * (bit_width / OWN_PARQUET_WIDTH);
                dst_ptr += max_count_lit * sizeof(uint32_t);
            }
            *pp_dst = dst_ptr;
            *pp_src = src_ptr;
            return status;
        }
        status = QPLC_STS_OK;
    }
    while ((src_ptr < src_stop_ptr) && (dst_ptr < dst_stop_ptr)) {
        kept_src_ptr = src_ptr;
        // Extract format and counter
        status       = ownc_decode_prle_header(&src_ptr, src_stop_ptr, &format, &count);
        if (status != QPLC_STS_OK) {
            src_ptr = kept_src_ptr;
            break;
        }
        if (OWN_LITERAL_OCTA_GROUP == format) {
            // This is a set of qplc_packed bit_width-integers (octa-groups)
            src_step = count * bit_width;
            dst_step = count * OWN_PARQUET_WIDTH * sizeof(uint32_t);
            if (((src_ptr + src_step) > src_stop_ptr) || ((dst_ptr + dst_step) > dst_stop_ptr)) {
                max_count_src = (uint32_t) (src_stop_ptr - src_ptr) / bit_width;
                max_count_dst = (uint32_t) ((dst_stop_ptr - dst_ptr) / (OWN_PARQUET_WIDTH * sizeof(uint32_t)));
                max_count     = QPL_MIN(max_count_src, max_count_dst);
                status        = (max_count == max_count_src) ? QPLC_STS_SRC_IS_SHORT_ERR : QPLC_STS_DST_IS_SHORT_ERR;
                *count_ptr = -(int32_t) (count - max_count);
                count = max_count;
                (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
                src_ptr += count * bit_width;
                dst_ptr += count * OWN_PARQUET_WIDTH * sizeof(uint32_t);
                break;
            }
            (*qplc_unpack_bits_array[bit_width - 1u])(src_ptr, count * OWN_PARQUET_WIDTH, 0u, dst_ptr);
            src_ptr += src_step;
            dst_ptr += dst_step;
        } else {
            // This is a set of RLE-qplc_packed bit_width-integers
            if ((src_ptr + value_width) > src_stop_ptr) {
                status  = QPLC_STS_SRC_IS_SHORT_ERR;
                src_ptr = kept_src_ptr;
                break;
            }
            if ((src_ptr + 3u) < src_stop_ptr) {
                value = (*(uint32_t *) src_ptr) & value_mask;
            } else {
                value = *(uint16_t *) src_ptr;
                value |= (*(src_ptr + 2u)) << OWN_WORD_WIDTH;
                value &= value_mask;
            }

            src_ptr += value_width;
            if ((dst_ptr + count * sizeof(uint32_t)) > dst_stop_ptr) {
                max_count = (uint32_t) ((dst_stop_ptr - dst_ptr) / sizeof(uint32_t));
                *count_ptr = (int32_t) (count - max_count);
                count = max_count;
                *value_ptr = value;
                status = QPLC_STS_DST_IS_SHORT_ERR;
            }
            CALL_CORE_FUN(qplc_set_32u)(value, (uint32_t *) dst_ptr, count);
            dst_ptr += count * sizeof(uint32_t);
        }
    }
    *pp_dst = dst_ptr;
    *pp_src = src_ptr;
    return status;
}
