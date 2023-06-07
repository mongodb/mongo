/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_TRIPLET_H_
#define QPL_TRIPLET_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct qpl_huffman_triplet
 * @brief Structure for intermediate representation of Huffman token
 */
typedef struct {
    uint8_t  value;          /**< Encoded value */
    uint8_t  code_length;    /**< Length of Huffman code for given value */
    uint16_t code;           /**< Huffman code for given value */
} qpl_huffman_triplet;

#ifdef __cplusplus
}
#endif

#endif //QPL_TRIPLET_H_
