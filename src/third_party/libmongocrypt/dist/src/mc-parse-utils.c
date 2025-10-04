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

#include "mc-parse-utils-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h"

bool parse_bindata(bson_subtype_t subtype, bson_iter_t *iter, _mongocrypt_buffer_t *out, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iter);
    BSON_ASSERT_PARAM(out);

    bson_subtype_t parsed_subtype;
    uint32_t len;
    const uint8_t *data;
    const char *field_name = bson_iter_key(iter);
    if (bson_iter_type(iter) != BSON_TYPE_BINARY) {
        CLIENT_ERR("Field '%s' expected to be bindata, got: %s",
                   field_name,
                   mc_bson_type_to_string(bson_iter_type(iter)));
        return false;
    }
    bson_iter_binary(iter, &parsed_subtype, &len, &data);
    if (parsed_subtype != subtype) {
        CLIENT_ERR("Field '%s' expected to be bindata subtype %d, got: %d",
                   field_name,
                   (int)subtype,
                   (int)parsed_subtype);
        return false;
    }
    if (!_mongocrypt_buffer_copy_from_binary_iter(out, iter)) {
        CLIENT_ERR("Unable to create mongocrypt buffer for BSON binary field in '%s'", field_name);
        return false;
    }
    return true;
}
