/*
 * Copyright 2025-present MongoDB, Inc.
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

#ifndef MC_PARSE_UTILS_PRIVATE_H
#define MC_PARSE_UTILS_PRIVATE_H

#include "mongocrypt-buffer-private.h"

/* Validates that the given bson_iter_t points to bindata element, with
 * the given subtype. If so, it copies the binary data to @out and returns true.
 * If validation fails, then it returns false and sets an error in @status.*/
bool parse_bindata(bson_subtype_t subtype, bson_iter_t *iter, _mongocrypt_buffer_t *out, mongocrypt_status_t *status);

#endif /* MC_PARSE_UTILS_PRIVATE_H */
