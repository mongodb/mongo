/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_HW_PATH_INCLUDE_OWN_COMPRESSION_CORES_API_H_
#define QPL_SOURCES_HW_PATH_INCLUDE_OWN_COMPRESSION_CORES_API_H_

#ifdef __cplusplus
extern "C" {
#endif

#define OWN_DEFLATE_LL_TABLE_SIZE 286u
#define OWN_DEFLATE_D_TABLE_SIZE  30u

extern const uint32_t fixed_literals_table[OWN_DEFLATE_LL_TABLE_SIZE];

extern const uint32_t fixed_offsets_table[OWN_DEFLATE_D_TABLE_SIZE];

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_HW_PATH_INCLUDE_OWN_COMPRESSION_CORES_API_H_
