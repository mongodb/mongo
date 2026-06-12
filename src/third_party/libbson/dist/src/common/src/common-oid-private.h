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

#include <common-prelude.h>

#ifndef MONGO_C_DRIVER_COMMON_OID_PRIVATE_H
#define MONGO_C_DRIVER_COMMON_OID_PRIVATE_H

#include <bson/bson.h>

BSON_BEGIN_DECLS

extern const bson_oid_t kZeroObjectId;

void
mcommon_oid_set_zero(bson_oid_t *oid);

bool
mcommon_oid_is_zero(const bson_oid_t *oid);

BSON_END_DECLS

#endif /* MONGO_C_DRIVER_COMMON_OID_PRIVATE_H */
