/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_OWN_QPL_STRUCTURES_H_
#define QPL_OWN_QPL_STRUCTURES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "igzip_lib.h"

/**
 *@brief Compression state struct definition
 *@note Currently this struct holds information
 * required only for middle-layer
 */
typedef struct {
    uint32_t middle_layer_compression_style;
} own_compression_state_t;

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_INCLUDE_OWN_QPL_STRUCTURES_H_
