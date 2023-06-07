/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @file
 * @brief functions related to bit buffer functionality
 *
 */

#ifndef OWN_BIT_WRITER_H_
#define OWN_BIT_WRITER_H_

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    available,
    overflowing
} writer_status_e;

typedef struct {
    uint64_t        buffer;          /**< Bits in the bit buffer */
    uint32_t        bits_in_buffer;  /**< Number of valid bits in the bit buffer */
    uint8_t         *current_ptr;    /**< Current index of buffer to write to */
    uint8_t         *end_ptr;        /**< End of buffer to write to */
    uint8_t         *start_ptr;      /**< Start of buffer to write to */
    writer_status_e status;          /**< Status of the writer */
} bit_writer_t;

uint32_t bit_writer_get_bits_used(bit_writer_t *const bit_writer_ptr);

uint32_t bit_writer_get_bytes_used(bit_writer_t *const bit_writer_ptr);

uint32_t bit_writer_get_available_bytes(const bit_writer_t *const bit_writer_ptr);

void bit_writer_init(bit_writer_t *const bit_writer_ptr);

void bit_writer_set_buffer(bit_writer_t *const bit_writer_ptr, const uint8_t *const buffer_ptr, const uint32_t length);

void bit_writer_flush(bit_writer_t *const bit_writer_ptr);

void bit_writer_write_bits(bit_writer_t *const bit_writer_ptr, const uint64_t code, const uint32_t code_length);

uint8_t bit_writer_available(bit_writer_t *const bit_writer_ptr);

#ifdef __cplusplus
}
#endif

#endif // OWN_BIT_WRITER_H_
