/*
 * Copyright 2018-present MongoDB, Inc.
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

#include "mc-fle-blob-subtype-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-log-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-status-private.h"
#include "mongocrypt-traverse-util-private.h"

typedef struct {
    void *ctx;
    bson_iter_t iter;
    bson_t *copy; /* implies transform */
    char *path;   /* only enabled during tracing. */
    _mongocrypt_traverse_callback_t traverse_cb;
    _mongocrypt_transform_callback_t transform_cb;
    mongocrypt_status_t *status;
    traversal_match_t match;
    bson_t child;
} _recurse_state_t;

static bool _check_first_byte(uint8_t byte, traversal_match_t match) {
    switch (match) {
    case TRAVERSE_MATCH_MARKING:
        return byte == MC_SUBTYPE_FLE1EncryptionPlaceholder || byte == MC_SUBTYPE_FLE2EncryptionPlaceholder;
    case TRAVERSE_MATCH_CIPHERTEXT:
        return byte == MC_SUBTYPE_FLE1DeterministicEncryptedValue || byte == MC_SUBTYPE_FLE1RandomEncryptedValue
            || byte == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue || byte == MC_SUBTYPE_FLE2UnindexedEncryptedValue
            || byte == MC_SUBTYPE_FLE2InsertUpdatePayload || byte == MC_SUBTYPE_FLE2IndexedRangeEncryptedValue
            || byte == MC_SUBTYPE_FLE2InsertUpdatePayloadV2 || byte == MC_SUBTYPE_FLE2UnindexedEncryptedValueV2
            || byte == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2
            || byte == MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2;
    case TRAVERSE_MATCH_SUBTYPE6: return true;
    default: break;
    }
    return false;
}

static bool _recurse(_recurse_state_t *state) {
    mongocrypt_status_t *status;

    BSON_ASSERT_PARAM(state);

    status = state->status;
    while (bson_iter_next(&state->iter)) {
        if (BSON_ITER_HOLDS_BINARY(&state->iter)) {
            _mongocrypt_buffer_t value;

            BSON_ASSERT(_mongocrypt_buffer_from_binary_iter(&value, &state->iter));

            if (value.subtype == BSON_SUBTYPE_ENCRYPTED && value.len > 0
                && _check_first_byte(value.data[0], state->match)) {
                bool ret;
                /* call the right callback. */
                if (state->copy) {
                    bson_value_t value_out;
                    ret = state->transform_cb(state->ctx, &value, &value_out, status);
                    if (ret) {
                        const uint32_t key_len = bson_iter_key_len(&state->iter);
                        BSON_ASSERT(key_len <= INT_MAX);
                        bson_append_value(state->copy, bson_iter_key(&state->iter), (int)key_len, &value_out);
                        bson_value_destroy(&value_out);
                    }
                } else {
                    ret = state->traverse_cb(state->ctx, &value, status);
                }

                if (!ret) {
                    return false;
                }
                continue;
            }
            /* fall through and copy */
        }

        if (BSON_ITER_HOLDS_ARRAY(&state->iter)) {
            _recurse_state_t child_state;
            bool ret;

            memcpy(&child_state, state, sizeof(_recurse_state_t));
            if (!bson_iter_recurse(&state->iter, &child_state.iter)) {
                CLIENT_ERR("error recursing into array");
                return false;
            }

            if (state->copy) {
                const uint32_t key_len = bson_iter_key_len(&state->iter);
                BSON_ASSERT(key_len <= INT_MAX);
                bson_append_array_begin(state->copy, bson_iter_key(&state->iter), (int)key_len, &state->child);
                child_state.copy = &state->child;
            }
            ret = _recurse(&child_state);

            if (state->copy) {
                bson_append_array_end(state->copy, &state->child);
            }
            if (!ret) {
                return false;
            }
            continue;
        }

        if (BSON_ITER_HOLDS_DOCUMENT(&state->iter)) {
            _recurse_state_t child_state;
            bool ret;

            memcpy(&child_state, state, sizeof(_recurse_state_t));
            if (!bson_iter_recurse(&state->iter, &child_state.iter)) {
                CLIENT_ERR("error recursing into document");
                return false;
            }
            /* TODO: check for errors everywhere. */
            if (state->copy) {
                const uint32_t key_len = bson_iter_key_len(&state->iter);
                BSON_ASSERT(key_len <= INT_MAX);
                bson_append_document_begin(state->copy, bson_iter_key(&state->iter), (int)key_len, &state->child);
                child_state.copy = &state->child;
            }

            ret = _recurse(&child_state);

            if (state->copy) {
                if (!bson_append_document_end(state->copy, &state->child)) {
                    CLIENT_ERR("error appending document");
                    return false;
                }
            }

            if (!ret) {
                return false;
            }
            continue;
        }

        if (state->copy) {
            const uint32_t key_len = bson_iter_key_len(&state->iter);
            BSON_ASSERT(key_len <= INT_MAX);
            bson_append_value(state->copy, bson_iter_key(&state->iter), (int)key_len, bson_iter_value(&state->iter));
        }
    }
    return true;
}

bool _mongocrypt_transform_binary_in_bson(_mongocrypt_transform_callback_t cb,
                                          void *ctx,
                                          traversal_match_t match,
                                          bson_iter_t *iter,
                                          bson_t *out,
                                          mongocrypt_status_t *status) {
    _recurse_state_t starting_state =
        {ctx, *iter, out /* copy */, NULL /* path */, NULL /* traverse callback */, cb, status, match, {0}};

    return _recurse(&starting_state);
}

/*-----------------------------------------------------------------------------
 *
 * _mongocrypt_traverse_binary_in_bson
 *
 *    Traverse the BSON being iterated with iter, and call cb for every binary
 *    subtype 06 (BSON_SUBTYPE_ENCRYPTED) value where the first byte corresponds
 *    to 'match'.
 *
 * Return:
 *    True on success. Returns false on failure and sets error.
 *
 *-----------------------------------------------------------------------------
 */
bool _mongocrypt_traverse_binary_in_bson(_mongocrypt_traverse_callback_t cb,
                                         void *ctx,
                                         traversal_match_t match,
                                         bson_iter_t *iter,
                                         mongocrypt_status_t *status) {
    _recurse_state_t starting_state =
        {ctx, *iter, NULL /* copy */, NULL /* path */, cb, NULL /* transform callback */, status, match, {0}};

    return _recurse(&starting_state);
}
