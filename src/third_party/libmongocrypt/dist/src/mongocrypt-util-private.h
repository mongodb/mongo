/*
 * Copyright 2021-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MONGOCRYPT_UTIL_PRIVATE_H
#define MONGOCRYPT_UTIL_PRIVATE_H

#include "mongocrypt-status-private.h"

#include "mlib/str.h"

#include <bson/bson.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A utility for safely casting from size_t to uint32_t.
 * Returns false if @in exceeds the maximum value of a uint32_t. */
bool size_to_uint32(size_t in, uint32_t *out);

/**
 * @brief The result type of mpath_current_exe_path()
 *
 * The @ref current_module_result::path member must be freed with mstr_free()
 */
typedef struct current_module_result {
    /// The resulting executable path
    mstr path;
    /// An error, if the path could not be obtained
    int error;
} current_module_result;

/**
 * @brief Obtain the path to the calling executable module
 *
 * If this function is contained in a dynamic library, this will return the path
 * to that library file, otherwise it will return the path to the running
 * executable.
 *
 * @return current_module_result A result object of the operation. Check the
 * `.error` member for non-zero. The `.path` member must be freed with
 * mtsr_free()
 */
current_module_result current_module_path(void);

/* mc_bson_type_to_string returns the string representation of a BSON type. */
const char *mc_bson_type_to_string(bson_type_t bson_type);

/* mc_iter_document_as_bson attempts to read the document from @iter into
 * @bson. */
bool mc_iter_document_as_bson(const bson_iter_t *iter, bson_t *bson, mongocrypt_status_t *status);

// mc_isnan is a wrapper around isnan. It avoids a conversion warning on glibc.
bool mc_isnan(double d);
// mc_isinf is a wrapper around isinf. It avoids a conversion warning on glibc.
bool mc_isinf(double d);
// mc_isfinite is a wrapper around isfinite. It avoids a conversion warning on
// glibc.
bool mc_isfinite(double d);

#endif /* MONGOCRYPT_UTIL_PRIVATE_H */
