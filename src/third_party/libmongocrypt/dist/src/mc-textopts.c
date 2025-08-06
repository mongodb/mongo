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

#include "mc-textopts-private.h"

#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_bson_type_to_string
#include "mongocrypt.h"
#include <bson/bson.h>

// Common logic for testing field name, tracking duplication, and presence.
#define IF_FIELD(Name)                                                                                                 \
    if (0 == strcmp(field, #Name)) {                                                                                   \
        if (has_##Name) {                                                                                              \
            CLIENT_ERR(ERROR_PREFIX "Unexpected duplicate field '" #Name "'");                                         \
            return false;                                                                                              \
        }                                                                                                              \
        has_##Name = true;                                                                                             \
    ((void)0)

#define END_IF_FIELD                                                                                                   \
    continue;                                                                                                          \
    }                                                                                                                  \
    else((void)0)

#define ERROR_PREFIX "Error parsing TextOpts: "

bool mc_TextOptsPerIndex_parse(mc_TextOptsPerIndex_t *txio, bson_iter_t *iter, mongocrypt_status_t *status) {
    *txio = (mc_TextOptsPerIndex_t){0};
    txio->set = true;

    bool has_strMaxLength = false, has_strMinQueryLength = false, has_strMaxQueryLength = false;
    while (bson_iter_next(iter)) {
        const char *field = bson_iter_key(iter);
        BSON_ASSERT(field);

        IF_FIELD(strMaxLength);
        {
            if (!BSON_ITER_HOLDS_INT32(iter)) {
                CLIENT_ERR(ERROR_PREFIX "'strMaxLength' must be an int32");
                return false;
            }
            const int32_t val = bson_iter_int32(iter);
            if (val <= 0) {
                CLIENT_ERR(ERROR_PREFIX "'strMaxLength' must be greater than zero");
                return false;
            }
            txio->strMaxLength = OPT_I32(val);
        }
        END_IF_FIELD;

        IF_FIELD(strMinQueryLength);
        {
            if (!BSON_ITER_HOLDS_INT32(iter)) {
                CLIENT_ERR(ERROR_PREFIX "'strMinQueryLength' must be an int32");
                return false;
            }
            const int32_t val = bson_iter_int32(iter);
            if (val <= 0) {
                CLIENT_ERR(ERROR_PREFIX "'strMinQueryLength' must be greater than zero");
                return false;
            }
            txio->strMinQueryLength = val;
        }
        END_IF_FIELD;

        IF_FIELD(strMaxQueryLength);
        {
            if (!BSON_ITER_HOLDS_INT32(iter)) {
                CLIENT_ERR(ERROR_PREFIX "'strMaxQueryLength' must be an int32");
                return false;
            }
            const int32_t val = bson_iter_int32(iter);
            if (val <= 0) {
                CLIENT_ERR(ERROR_PREFIX "'strMaxQueryLength' must be greater than zero");
                return false;
            }
            txio->strMaxQueryLength = val;
        }
        END_IF_FIELD;

        CLIENT_ERR(ERROR_PREFIX "Unrecognized field: '%s'", field);
        return false;
    }
    return true;
}

bool mc_TextOpts_parse(mc_TextOpts_t *txo, const bson_t *in, mongocrypt_status_t *status) {
    bson_iter_t iter = {0};
    BSON_ASSERT_PARAM(txo);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT(status || true);
    bool has_caseSensitive = false, has_diacriticSensitive = false, has_substring = false, has_prefix = false,
         has_suffix = false;

    *txo = (mc_TextOpts_t){{0}};
    if (!bson_iter_init(&iter, in)) {
        CLIENT_ERR(ERROR_PREFIX "Invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field = bson_iter_key(&iter);
        IF_FIELD(caseSensitive);
        {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "Expected bool for caseSensitive, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                return false;
            }
            txo->caseSensitive = bson_iter_bool(&iter);
        }
        END_IF_FIELD;

        IF_FIELD(diacriticSensitive);
        {
            if (!BSON_ITER_HOLDS_BOOL(&iter)) {
                CLIENT_ERR(ERROR_PREFIX "Expected bool for diacriticSensitive, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                return false;
            }
            txo->diacriticSensitive = bson_iter_bool(&iter);
        }
        END_IF_FIELD;

        IF_FIELD(substring);
        {
            bson_iter_t subdoc;
            if (!BSON_ITER_HOLDS_DOCUMENT(&iter) || !bson_iter_recurse(&iter, &subdoc)) {
                CLIENT_ERR(ERROR_PREFIX "Expected document for substring, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                return false;
            }

            if (!mc_TextOptsPerIndex_parse(&txo->substring, &subdoc, status)) {
                return false;
            }

            if (!txo->substring.strMaxLength.set) {
                CLIENT_ERR(ERROR_PREFIX "'strMaxLength' must be set for substring");
                return false;
            }
        }
        END_IF_FIELD;

        IF_FIELD(prefix);
        {
            bson_iter_t subdoc;
            if (!BSON_ITER_HOLDS_DOCUMENT(&iter) || !bson_iter_recurse(&iter, &subdoc)) {
                CLIENT_ERR(ERROR_PREFIX "Expected document for prefix, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                return false;
            }

            if (!mc_TextOptsPerIndex_parse(&txo->prefix, &subdoc, status)) {
                return false;
            }

            if (txo->prefix.strMaxLength.set) {
                CLIENT_ERR(ERROR_PREFIX "'strMaxLength' is not allowed in 'prefix'");
                return false;
            }
        }
        END_IF_FIELD;

        IF_FIELD(suffix);
        {
            bson_iter_t subdoc;
            if (!BSON_ITER_HOLDS_DOCUMENT(&iter) || !bson_iter_recurse(&iter, &subdoc)) {
                CLIENT_ERR(ERROR_PREFIX "Expected document for suffix, got: %s",
                           mc_bson_type_to_string(bson_iter_type(&iter)));
                return false;
            }

            if (!mc_TextOptsPerIndex_parse(&txo->suffix, &subdoc, status)) {
                return false;
            }

            if (txo->suffix.strMaxLength.set) {
                CLIENT_ERR(ERROR_PREFIX "'strMaxLength' is not allowed in 'suffix'");
                return false;
            }
        }
        END_IF_FIELD;

        CLIENT_ERR(ERROR_PREFIX "Unrecognized field: '%s'", field);
        return false;
    }

    if (!has_caseSensitive) {
        CLIENT_ERR(ERROR_PREFIX "'caseSensitive' is required");
        return false;
    }
    if (!has_diacriticSensitive) {
        CLIENT_ERR(ERROR_PREFIX "'diacriticSensitive' is required");
        return false;
    }
    if (has_substring && (has_prefix || has_suffix)) {
        CLIENT_ERR(ERROR_PREFIX "Cannot specify 'substring' with 'prefix' or 'suffix'");
        return false;
    }
    if (!(has_prefix || has_suffix || has_substring)) {
        CLIENT_ERR(ERROR_PREFIX "One of 'prefix', 'suffix', or 'substring' is required");
        return false;
    }

    return true;
}

#undef ERROR_PREFIX
#define ERROR_PREFIX "Error making FLE2RangeInsertSpec: "

static bool append_TextOptsPerIndex(const mc_TextOptsPerIndex_t *txio, bson_t *out, mongocrypt_status_t *status) {
    if (txio->strMaxLength.set) {
        if (!bson_append_int32(out, "mlen", -1, txio->strMaxLength.value)) {
            CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
            return false;
        }
    }

    if (!bson_append_int32(out, "ub", -1, txio->strMaxQueryLength)) {
        CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
        return false;
    }

    if (!bson_append_int32(out, "lb", -1, txio->strMinQueryLength)) {
        CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
        return false;
    }
    return true;
}

bool mc_TextOpts_to_FLE2TextSearchInsertSpec(const mc_TextOpts_t *txo,
                                             const bson_t *v,
                                             bson_t *out,
                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(txo);
    BSON_ASSERT_PARAM(v);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(status || true);

    bson_iter_t v_iter;
    if (!bson_iter_init_find(&v_iter, v, "v")) {
        CLIENT_ERR(ERROR_PREFIX "Unable to find 'v' in input");
        return false;
    }

    bson_t child;
    if (!BSON_APPEND_DOCUMENT_BEGIN(out, "v", &child)) {
        CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
        return false;
    }
    if (!bson_append_iter(&child, "v", 1, &v_iter)) {
        CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
        return false;
    }
    if (!bson_append_bool(&child, "casef", -1, txo->caseSensitive)) {
        CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
        return false;
    }

    if (!bson_append_bool(&child, "diacf", -1, txo->diacriticSensitive)) {
        CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
        return false;
    }

    if (txo->prefix.set) {
        bson_t insert_spec;
        if (!BSON_APPEND_DOCUMENT_BEGIN(&child, "prefix", &insert_spec)) {
            CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
            return false;
        }

        if (!append_TextOptsPerIndex(&txo->prefix, &insert_spec, status)) {
            return false;
        }

        if (!bson_append_document_end(&child, &insert_spec)) {
            CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
            return false;
        }
    }

    if (txo->suffix.set) {
        bson_t insert_spec;
        if (!BSON_APPEND_DOCUMENT_BEGIN(&child, "suffix", &insert_spec)) {
            CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
            return false;
        }

        if (!append_TextOptsPerIndex(&txo->suffix, &insert_spec, status)) {
            return false;
        }

        if (!bson_append_document_end(&child, &insert_spec)) {
            CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
            return false;
        }
    }

    if (txo->substring.set) {
        bson_t insert_spec;
        if (!BSON_APPEND_DOCUMENT_BEGIN(&child, "substr", &insert_spec)) {
            CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
            return false;
        }

        if (!append_TextOptsPerIndex(&txo->substring, &insert_spec, status)) {
            return false;
        }

        if (!bson_append_document_end(&child, &insert_spec)) {
            CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
            return false;
        }
    }

    if (!bson_append_document_end(out, &child)) {
        CLIENT_ERR(ERROR_PREFIX "Error appending to BSON");
        return false;
    }

    return true;
}

#undef ERROR_PREFIX
