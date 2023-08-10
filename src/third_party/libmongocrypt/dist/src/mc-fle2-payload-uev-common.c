/*
 * Copyright 2023-present MongoDB, Inc.
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

#include "mc-fle2-payload-uev-common-private.h"
#include "mc-reader-private.h"
#include "mongocrypt-private.h"

#define CHECK_AND_RETURN(x)                                                                                            \
    if (!(x)) {                                                                                                        \
        return false;                                                                                                  \
    }

bool _mc_FLE2UnindexedEncryptedValueCommon_parse(const _mongocrypt_buffer_t *buf,
                                                 uint8_t *fle_blob_subtype,
                                                 uint8_t *original_bson_type,
                                                 _mongocrypt_buffer_t *key_uuid,
                                                 _mongocrypt_buffer_t *ciphertext,
                                                 mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(fle_blob_subtype);
    BSON_ASSERT_PARAM(original_bson_type);
    BSON_ASSERT_PARAM(key_uuid);
    BSON_ASSERT_PARAM(ciphertext);

    mc_reader_t reader;
    mc_reader_init_from_buffer(&reader, buf, __FUNCTION__);

    /* Read fle_blob_subtype. */
    CHECK_AND_RETURN(mc_reader_read_u8(&reader, fle_blob_subtype, status));

    /* Read key_uuid. */
    CHECK_AND_RETURN(mc_reader_read_buffer(&reader, key_uuid, 16, status));
    key_uuid->subtype = BSON_SUBTYPE_UUID;

    /* Read original_bson_type. */
    CHECK_AND_RETURN(mc_reader_read_u8(&reader, original_bson_type, status));

    /* Read ciphertext. */
    CHECK_AND_RETURN(mc_reader_read_buffer(&reader, ciphertext, mc_reader_get_remaining_length(&reader), status));

    return true;
}

const _mongocrypt_buffer_t *_mc_FLE2UnindexedEncryptedValueCommon_decrypt(_mongocrypt_crypto_t *crypto,
                                                                          mc_fle_blob_subtype_t fle_blob_subtype,
                                                                          const _mongocrypt_buffer_t *key_uuid,
                                                                          bson_type_t original_bson_type,
                                                                          const _mongocrypt_buffer_t *ciphertext,
                                                                          const _mongocrypt_buffer_t *key,
                                                                          _mongocrypt_buffer_t *plaintext,
                                                                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(key_uuid);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(plaintext);

    BSON_ASSERT(MC_SUBTYPE_FLE2UnindexedEncryptedValue == fle_blob_subtype
                || MC_SUBTYPE_FLE2UnindexedEncryptedValueV2 == fle_blob_subtype);

    const _mongocrypt_value_encryption_algorithm_t *fle2aead =
        (MC_SUBTYPE_FLE2UnindexedEncryptedValue == fle_blob_subtype) ? _mcFLE2AEADAlgorithm()
                                                                     : _mcFLE2v2AEADAlgorithm();

    /* Serialize associated data: fle_blob_subtype || key_uuid ||
     * original_bson_type */
    _mongocrypt_buffer_t AD;
    _mongocrypt_buffer_init(&AD);
    if (key_uuid->len > UINT32_MAX - 2) {
        CLIENT_ERR("mc_FLE2UnindexedEncryptedValueCommon_decrypt expected "
                   "key UUID length <= %" PRIu32 " got: %" PRIu32,
                   UINT32_MAX - 2u,
                   key_uuid->len);
        return NULL;
    }
    _mongocrypt_buffer_resize(&AD, 1 + key_uuid->len + 1);

    AD.data[0] = (uint8_t)fle_blob_subtype;
    memcpy(AD.data + 1, key_uuid->data, key_uuid->len);
    AD.data[1 + key_uuid->len] = (uint8_t)original_bson_type;
    const uint32_t plaintext_len = fle2aead->get_plaintext_len(ciphertext->len, status);
    if (plaintext_len == 0) {
        _mongocrypt_buffer_cleanup(&AD);
        return NULL;
    }
    _mongocrypt_buffer_resize(plaintext, plaintext_len);

    uint32_t bytes_written;

    if (!fle2aead->do_decrypt(crypto, &AD, key, ciphertext, plaintext, &bytes_written, status)) {
        _mongocrypt_buffer_cleanup(&AD);
        return NULL;
    }

    // Some block cipher modes (eg. CBC) may write fewer bytes than the size
    // estimate that the plaintext buffer was allocated with. Therefore, the
    // plaintext buffer length must be updated to the actual size written.
    plaintext->len = bytes_written;

    _mongocrypt_buffer_cleanup(&AD);
    return plaintext;
}

bool _mc_FLE2UnindexedEncryptedValueCommon_encrypt(_mongocrypt_crypto_t *crypto,
                                                   mc_fle_blob_subtype_t fle_blob_subtype,
                                                   const _mongocrypt_buffer_t *key_uuid,
                                                   bson_type_t original_bson_type,
                                                   const _mongocrypt_buffer_t *plaintext,
                                                   const _mongocrypt_buffer_t *key,
                                                   _mongocrypt_buffer_t *out,
                                                   mongocrypt_status_t *status) {
    _mongocrypt_buffer_t iv = {0};
    _mongocrypt_buffer_t AD = {0};
    bool res = false;

    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(key_uuid);
    BSON_ASSERT_PARAM(plaintext);
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(out);

    BSON_ASSERT(MC_SUBTYPE_FLE2UnindexedEncryptedValue == fle_blob_subtype
                || MC_SUBTYPE_FLE2UnindexedEncryptedValueV2 == fle_blob_subtype);

    const _mongocrypt_value_encryption_algorithm_t *fle2aead =
        (MC_SUBTYPE_FLE2UnindexedEncryptedValue == fle_blob_subtype) ? _mcFLE2AEADAlgorithm()
                                                                     : _mcFLE2v2AEADAlgorithm();

    _mongocrypt_buffer_resize(&iv, MONGOCRYPT_IV_LEN);
    if (!_mongocrypt_random(crypto, &iv, MONGOCRYPT_IV_LEN, status)) {
        goto fail;
    }

    /* Serialize associated data: fle_blob_subtype || key_uuid ||
     * original_bson_type */
    {
        if (key_uuid->len > UINT32_MAX - 2) {
            CLIENT_ERR("mc_FLE2UnindexedEncryptedValueCommon_encrypt expected "
                       "key UUID length <= %" PRIu32 " got: %" PRIu32,
                       UINT32_MAX - 2u,
                       key_uuid->len);
            goto fail;
        }
        _mongocrypt_buffer_resize(&AD, 1 + key_uuid->len + 1);
        AD.data[0] = (uint8_t)fle_blob_subtype;
        memcpy(AD.data + 1, key_uuid->data, key_uuid->len);
        AD.data[1 + key_uuid->len] = (uint8_t)original_bson_type;
    }

    /* Encrypt. */
    {
        const uint32_t cipherlen = fle2aead->get_ciphertext_len(plaintext->len, status);
        if (cipherlen == 0) {
            goto fail;
        }
        _mongocrypt_buffer_resize(out, cipherlen);
        uint32_t bytes_written; /* unused. */
        if (!fle2aead->do_encrypt(crypto, &iv, &AD, key, plaintext, out, &bytes_written, status)) {
            goto fail;
        }
    }

    res = true;

fail:
    _mongocrypt_buffer_cleanup(&AD);
    _mongocrypt_buffer_cleanup(&iv);
    return res;
}
