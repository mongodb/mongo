/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @file
 * @brief Intel® Query Processing Library (Intel® QPL) internal functions for working with bit buffer
 */

/* ------ Includes ------ */

#include "bit_writer.h"
#include "own_defs.h"

#define _mm_stream_si64x(dst, src) *((uint64_t *)dst) = src

/* ------ Internal functions implementation ------ */

static inline void own_bit_writer_flush_bits(bit_writer_t *const bit_writer_ptr) {
    // Variables
    const uint32_t number_of_bits  = bit_writer_ptr->bits_in_buffer & ~7u;
    const uint32_t completeBytes = number_of_bits / OWN_BYTE_BIT_LEN;

    // Writing the whole 64-bit buffer into output
    _mm_stream_si64x((int64_t *) bit_writer_ptr->current_ptr, bit_writer_ptr->buffer);

    // Simple assignment
    bit_writer_ptr->bits_in_buffer -= number_of_bits;
    bit_writer_ptr->current_ptr += completeBytes;
    bit_writer_ptr->buffer >>= number_of_bits;
}

/* ------ Own bit-writer functions implementation ------ */

uint32_t bit_writer_get_bits_used(bit_writer_t *const bit_writer_ptr) {
    return (8u * (uint32_t) (bit_writer_ptr->current_ptr - bit_writer_ptr->start_ptr) + bit_writer_ptr->bits_in_buffer);
}

uint32_t bit_writer_get_bytes_used(bit_writer_t *const bit_writer_ptr) {
    return (uint32_t) (bit_writer_ptr->current_ptr - bit_writer_ptr->start_ptr);
}

uint32_t bit_writer_get_available_bytes(const bit_writer_t *const bit_writer_ptr) {
    return (uint32_t) (bit_writer_ptr->end_ptr - bit_writer_ptr->start_ptr) + 8u;
}

uint8_t bit_writer_available(bit_writer_t *const bit_writer_ptr) {
    return available == bit_writer_ptr->status;
}

void bit_writer_init(bit_writer_t *const bit_writer_ptr) {
    bit_writer_ptr->buffer         = 0u;
    bit_writer_ptr->bits_in_buffer = 0u;
}

void bit_writer_set_buffer(bit_writer_t *const bit_writer_ptr, const uint8_t *const buffer_ptr, const uint32_t length) {
    // Variables
    uint32_t slop = 8u;

    // Simple assignment
    bit_writer_ptr->current_ptr = (uint8_t *) (buffer_ptr);
    bit_writer_ptr->start_ptr   = (uint8_t *) (buffer_ptr);
    bit_writer_ptr->end_ptr     = (uint8_t *) (buffer_ptr + length - slop);
    bit_writer_ptr->status   = available;
}

void bit_writer_flush(bit_writer_t *const bit_writer_ptr) {
    // Variables
    uint32_t bytes;

    // Main actions
    if (available == bit_writer_ptr->status)
    {
        if (bit_writer_ptr->bits_in_buffer)
        {
            _mm_stream_si64x((int64_t *) bit_writer_ptr->current_ptr, bit_writer_ptr->buffer);
            bytes = (bit_writer_ptr->bits_in_buffer + 7u) / 8u;
            bit_writer_ptr->current_ptr += bytes;
        }

        bit_writer_ptr->buffer         = 0u;
        bit_writer_ptr->bits_in_buffer = 0u;
    }
}

void bit_writer_write_bits(bit_writer_t *const bit_writer_ptr, const uint64_t code, const uint32_t code_length) {
    if (available == bit_writer_ptr->status)
    {
        // Remembering new information
        bit_writer_ptr->buffer |= code << bit_writer_ptr->bits_in_buffer;
        bit_writer_ptr->bits_in_buffer += code_length;

        // Variables for safety check
        const uint32_t bits          = bit_writer_ptr->bits_in_buffer & ~7u;
        const uint32_t completeBytes = bits / 8u;

        // Performing safe writing
        if (bit_writer_ptr->current_ptr + completeBytes >= bit_writer_ptr->end_ptr)
        {
            bit_writer_ptr->status = overflowing;
        }
        else
        {
            own_bit_writer_flush_bits(bit_writer_ptr);
        }
    }
}
