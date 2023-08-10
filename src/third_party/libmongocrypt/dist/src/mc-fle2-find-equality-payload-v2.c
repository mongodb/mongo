/*
 * Copyright 2022-present MongoDB, Inc.
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

#include <bson/bson.h>

#include "mc-fle2-find-equality-payload-private-v2.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt.h"

void mc_FLE2FindEqualityPayloadV2_init(mc_FLE2FindEqualityPayloadV2_t *payload) {
    memset(payload, 0, sizeof(mc_FLE2FindEqualityPayloadV2_t));
}

void mc_FLE2FindEqualityPayloadV2_cleanup(mc_FLE2FindEqualityPayloadV2_t *payload) {
    BSON_ASSERT_PARAM(payload);

    _mongocrypt_buffer_cleanup(&payload->edcDerivedToken);
    _mongocrypt_buffer_cleanup(&payload->escDerivedToken);
    _mongocrypt_buffer_cleanup(&payload->serverDerivedFromDataToken);
}

#define IF_FIELD(Name)                                                                                                 \
    if (0 == strcmp(field, #Name)) {                                                                                   \
        if (has_##Name) {                                                                                              \
            CLIENT_ERR("Duplicate field '" #Name "' in payload bson");                                                 \
            goto fail;                                                                                                 \
        }                                                                                                              \
        has_##Name = true;

#define END_IF_FIELD                                                                                                   \
    continue;                                                                                                          \
    }

#define PARSE_BINARY(Name, Dest)                                                                                       \
    IF_FIELD(Name) {                                                                                                   \
        bson_subtype_t subtype;                                                                                        \
        uint32_t len;                                                                                                  \
        const uint8_t *data;                                                                                           \
        if (bson_iter_type(&iter) != BSON_TYPE_BINARY) {                                                               \
            CLIENT_ERR("Field '" #Name "' expected to be bindata, got: %d", bson_iter_type(&iter));                    \
            goto fail;                                                                                                 \
        }                                                                                                              \
        bson_iter_binary(&iter, &subtype, &len, &data);                                                                \
        if (subtype != BSON_SUBTYPE_BINARY) {                                                                          \
            CLIENT_ERR("Field '" #Name "' expected to be bindata subtype %d, got: %d", BSON_SUBTYPE_BINARY, subtype);  \
            goto fail;                                                                                                 \
        }                                                                                                              \
        if (!_mongocrypt_buffer_copy_from_binary_iter(&out->Dest, &iter)) {                                            \
            CLIENT_ERR("Unable to create mongocrypt buffer for BSON binary "                                           \
                       "field in '" #Name "'");                                                                        \
            goto fail;                                                                                                 \
        }                                                                                                              \
    }                                                                                                                  \
    END_IF_FIELD

#define CHECK_HAS(Name)                                                                                                \
    if (!has_##Name) {                                                                                                 \
        CLIENT_ERR("Missing field '" #Name "' in payload");                                                            \
        goto fail;                                                                                                     \
    }

bool mc_FLE2FindEqualityPayloadV2_parse(mc_FLE2FindEqualityPayloadV2_t *out,
                                        const bson_t *in,
                                        mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_d = false, has_s = false, has_l = false, has_cm = false;

    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    mc_FLE2FindEqualityPayloadV2_init(out);
    if (!bson_validate(in, BSON_VALIDATE_NONE, NULL) || !bson_iter_init(&iter, in)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        BSON_ASSERT(field);

        PARSE_BINARY(d, edcDerivedToken)
        PARSE_BINARY(s, escDerivedToken)
        PARSE_BINARY(l, serverDerivedFromDataToken)
        IF_FIELD(cm) {
            if (!BSON_ITER_HOLDS_INT64(&iter)) {
                CLIENT_ERR("Field 'cm' expected to hold an int64");
                goto fail;
            }
            out->maxContentionCounter = bson_iter_int64(&iter);
        }
        END_IF_FIELD
    }

    CHECK_HAS(d);
    CHECK_HAS(s);
    CHECK_HAS(l);
    CHECK_HAS(cm);

    return true;
fail:
    mc_FLE2FindEqualityPayloadV2_cleanup(out);
    return false;
}

#define APPEND_BINDATA(name, value)                                                                                    \
    if (!_mongocrypt_buffer_append(&(value), out, name, -1)) {                                                         \
        return false;                                                                                                  \
    }

bool mc_FLE2FindEqualityPayloadV2_serialize(const mc_FLE2FindEqualityPayloadV2_t *payload, bson_t *out) {
    BSON_ASSERT_PARAM(payload);

    APPEND_BINDATA("d", payload->edcDerivedToken);
    APPEND_BINDATA("s", payload->escDerivedToken);
    APPEND_BINDATA("l", payload->serverDerivedFromDataToken);
    if (!BSON_APPEND_INT64(out, "cm", payload->maxContentionCounter)) {
        return false;
    }
    return true;
}

#undef APPEND_BINDATA
