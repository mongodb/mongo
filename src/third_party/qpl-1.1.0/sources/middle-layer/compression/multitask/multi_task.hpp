/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_MULTITASK_MULTITASK_HPP_
#define QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_MULTITASK_MULTITASK_HPP_

namespace qpl::ml::util {
    enum multitask_status : uint32_t {
        ready                   = 0u,
        multi_chunk_in_progress = 0u,
        multi_chunk_last_chunk  = 1u,
        multi_chunk_first_chunk = 2u,
        single_chunk_processing = 3u,
    };

    extern std::array<uint16_t, 4u> aecs_decompress_access_lookup_table;
    extern std::array<uint16_t, 4u> aecs_verify_access_lookup_table;
    extern std::array<uint16_t, 4u> aecs_compress_access_lookup_table;
}

#endif //QPL_SOURCES_MIDDLE_LAYER_COMPRESSION_MULTITASK_MULTITASK_HPP_
