/*
 * Copyright 2009-present MongoDB, Inc.
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

#include <bson/bson-prelude.h>


#ifndef BSON_ISO8601_PRIVATE_H
#define BSON_ISO8601_PRIVATE_H


#include <common-string-private.h>

#include <bson/compat.h>
#include <bson/macros.h>


BSON_BEGIN_DECLS

bool
_bson_iso8601_date_parse(const char *str, int32_t len, int64_t *out, bson_error_t *error);

BSON_END_DECLS


#endif /* BSON_ISO8601_PRIVATE_H */
