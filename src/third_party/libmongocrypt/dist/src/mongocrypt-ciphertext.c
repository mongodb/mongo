/*
 * Copyright 2019-present MongoDB, Inc.
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

#include "mongocrypt-ciphertext-private.h"
#include "mongocrypt-private.h"

void _mongocrypt_ciphertext_init(_mongocrypt_ciphertext_t *ciphertext) {
    BSON_ASSERT_PARAM(ciphertext);

    memset(ciphertext, 0, sizeof(*ciphertext));
}

void _mongocrypt_ciphertext_cleanup(_mongocrypt_ciphertext_t *ciphertext) {
    if (!ciphertext) {
        return;
    }

    _mongocrypt_buffer_cleanup(&ciphertext->key_id);
    _mongocrypt_buffer_cleanup(&ciphertext->data);
}

bool _mongocrypt_ciphertext_parse_unowned(_mongocrypt_buffer_t *in,
                                          _mongocrypt_ciphertext_t *ciphertext,
                                          mongocrypt_status_t *status) {
    uint32_t offset;

    /* From BSON Binary subtype 6 specification:
       struct fle_blob {
       uint8  fle_blob_subtype = (1 or 2);
       uint8  key_uuid[16];
       uint8  original_bson_type;
       uint8  ciphertext[ciphertext_length];
       }
    */

    if (!ciphertext) {
        CLIENT_ERR("ciphertext cannot be null");
        return false;
    }

    if (!in) {
        CLIENT_ERR("in parameter cannot be null");
        return false;
    }

    if (!status) {
        CLIENT_ERR("status cannot be null");
        return false;
    }

    offset = 0;

    /* At a minimum, a ciphertext must be 19 bytes:
     * fle_blob_subtype (1) +
     * key_uuid (16) +
     * original_bson_type (1) +
     * ciphertext (> 0)
     */
    if (in->len < 19) {
        CLIENT_ERR("malformed ciphertext, too small");
        return false;
    }

    ciphertext->blob_subtype = in->data[0];
    offset += 1;

    /* TODO: merge new changes. */
    if (ciphertext->blob_subtype != 1 && ciphertext->blob_subtype != 2) {
        CLIENT_ERR("malformed ciphertext, expected blob subtype of 1 or 2");
        return false;
    }

    _mongocrypt_buffer_init(&ciphertext->key_id);
    ciphertext->key_id.data = in->data + offset;
    ciphertext->key_id.len = 16;
    ciphertext->key_id.subtype = BSON_SUBTYPE_UUID;
    offset += 16;

    ciphertext->original_bson_type = in->data[offset];
    offset += 1;

    memset(&ciphertext->data, 0, sizeof(ciphertext->data));
    ciphertext->data.data = in->data + offset;
    ciphertext->data.len = in->len - offset;

    return true;
}

bool _mongocrypt_serialize_ciphertext(_mongocrypt_ciphertext_t *ciphertext, _mongocrypt_buffer_t *out) {
    uint32_t offset;

    /* From BSON Binary subtype 6 specification:
       struct fle_blob {
       uint8  fle_blob_subtype = (1 or 2);
       uint8  key_uuid[16];
       uint8  original_bson_type;
       uint8  ciphertext[ciphertext_length];
       }
    */

    if (!ciphertext || !out) {
        return false;
    }

    if (ciphertext->key_id.len != 16) {
        return false;
    }
    if (ciphertext->key_id.len > (UINT32_MAX - ciphertext->data.len - 1)
        || ciphertext->key_id.len > (SIZE_MAX - ciphertext->data.len - 1)) {
        return false;
    }

    _mongocrypt_buffer_init(out);
    offset = 0;
    out->len = 1 + ciphertext->key_id.len + 1 + ciphertext->data.len;
    out->data = bson_malloc0(out->len);
    BSON_ASSERT(out->data);

    out->owned = true;

    /* ciphertext->blob_subtype is an enum and easily fits in uint8_t */
    out->data[offset] = (uint8_t)ciphertext->blob_subtype;
    offset += 1;

    memcpy(out->data + offset, ciphertext->key_id.data, ciphertext->key_id.len);
    offset += ciphertext->key_id.len;

    out->data[offset] = ciphertext->original_bson_type;
    offset += 1;

    memcpy(out->data + offset, ciphertext->data.data, ciphertext->data.len);

    return true;
}

/*
From "FLE and AEAD" doc:
A = Associated Data = fle_blob_subtype + key_uuid[16] + original_bson_type
*/

bool _mongocrypt_ciphertext_serialize_associated_data(_mongocrypt_ciphertext_t *ciphertext, _mongocrypt_buffer_t *out) {
    BSON_ASSERT_PARAM(ciphertext);

    uint32_t bytes_written = 0;

    if (!out) {
        return false;
    }

    _mongocrypt_buffer_init(out);

    if (!ciphertext->original_bson_type) {
        return false;
    }

    if (!_mongocrypt_buffer_is_uuid(&ciphertext->key_id)) {
        return false;
    }

    if ((ciphertext->blob_subtype != MC_SUBTYPE_FLE1DeterministicEncryptedValue)
        && (ciphertext->blob_subtype != MC_SUBTYPE_FLE1RandomEncryptedValue)) {
        return false;
    }

    if (ciphertext->key_id.len > (UINT32_MAX - 2)) {
        return false;
    }

    out->len = 1 + ciphertext->key_id.len + 1;
    out->data = bson_malloc(out->len);
    BSON_ASSERT(out->data);
    out->owned = true;

    out->data[bytes_written++] = (uint8_t)ciphertext->blob_subtype;
    memcpy(out->data + bytes_written, ciphertext->key_id.data, ciphertext->key_id.len);
    bytes_written += ciphertext->key_id.len;
    out->data[bytes_written++] = (uint8_t)ciphertext->original_bson_type;

    return true;
}
