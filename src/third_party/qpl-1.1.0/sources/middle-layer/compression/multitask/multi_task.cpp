/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#include "array"
#include "hw_aecs_api.h"

namespace qpl::ml::util {
    std::array<uint16_t, 4u> aecs_decompress_access_lookup_table = {
            hw_aecs_access_read | hw_aecs_access_write,        // Multi Chunk Mode: In progress
            hw_aecs_access_read | hw_aecs_access_maybe_write,  // Multi Chunk Mode: Last Chunk
            hw_aecs_access_write,                              // Multi Chunk Mode: First Chunk
            hw_aecs_access_maybe_write                         // Single Chunk Mode
    };

    std::array<uint16_t, 4u> aecs_verify_access_lookup_table = {
            hw_aecs_access_read | hw_aecs_access_write,        // Multi Chunk Mode: In progress
            hw_aecs_access_read,                               // Multi Chunk Mode: Last Chunk
            hw_aecs_access_write,                              // Multi Chunk Mode: First Chunk
            0u                                                 // Single Chunk Mode
    };

    std::array<uint16_t, 4u> aecs_compress_access_lookup_table = {
            hw_aecs_access_read | hw_aecs_access_write,        // Multi Chunk Mode: In progress
            hw_aecs_access_read,                               // Multi Chunk Mode: Last Chunk
            hw_aecs_access_read | hw_aecs_access_write,        // Multi Chunk Mode: First Chunk
            hw_aecs_access_read                                // Single Chunk Mode
    };
}
