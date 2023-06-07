/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_SERIALIZATION_H_
#define QPL_SERIALIZATION_H_

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup SERIALIZATION_API Serialization API
 * @ingroup JOB_API
 * @{
 */

/**
 * @enum qpl_serialization_format_e
 * Describes how to perform serialization
 */
typedef enum {
    serialization_compact,   /**< More compact representation, useful for saving up memory on the disk */
    serialization_raw,       /**< Faster but more straightforward implementation, use to save speed of serialization/deserialization in case high load */
} qpl_serialization_format_e;

typedef uint64_t serialization_flags_t; /**< Type of serialization flags */

/**
 * @struct serialization_options_t
 * @brief Describes serialization options
 */
typedef struct {
    qpl_serialization_format_e format;  /**< @ref qpl_serialization_format_e of serialized object */
    serialization_flags_t flags;        /**< Advanced serialization options, placeholder for later */
} serialization_options_t;

#define DEFAULT_SERIALIZATION_OPTIONS {serialization_raw, 0} /**< Default serialization options */

/** @} */

#ifdef __cplusplus
}
#endif

#endif //QPL_SERIALIZATION_H_
