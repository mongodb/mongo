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

#include "mc-fle2-payload-uev-common-private.h"
#include "mc-fle2-payload-uev-private.h"
#include "mongocrypt-private.h"

struct _mc_FLE2UnindexedEncryptedValue_t {
    _mongocrypt_buffer_t key_uuid;
    uint8_t original_bson_type;
    _mongocrypt_buffer_t ciphertext;
    _mongocrypt_buffer_t plaintext;
    bool parsed;
};

mc_FLE2UnindexedEncryptedValue_t *mc_FLE2UnindexedEncryptedValue_new(void) {
    mc_FLE2UnindexedEncryptedValue_t *uev = bson_malloc0(sizeof(mc_FLE2UnindexedEncryptedValue_t));
    return uev;
}

bool mc_FLE2UnindexedEncryptedValue_parse(mc_FLE2UnindexedEncryptedValue_t *uev,
                                          const _mongocrypt_buffer_t *buf,
                                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(uev);
    BSON_ASSERT_PARAM(buf);

    if (uev->parsed) {
        CLIENT_ERR("mc_FLE2UnindexedEncryptedValue_parse must not be called twice");
        return false;
    }

    uint8_t fle_blob_subtype;

    if (!_mc_FLE2UnindexedEncryptedValueCommon_parse(buf,
                                                     &fle_blob_subtype,
                                                     &uev->original_bson_type,
                                                     &uev->key_uuid,
                                                     &uev->ciphertext,
                                                     status)) {
        return false;
    }

    if (fle_blob_subtype != MC_SUBTYPE_FLE2UnindexedEncryptedValue) {
        CLIENT_ERR("mc_FLE2UnindexedEncryptedValue_parse expected "
                   "fle_blob_subtype=%d got: %" PRIu8,
                   MC_SUBTYPE_FLE2UnindexedEncryptedValue,
                   fle_blob_subtype);
        return false;
    }

    uev->parsed = true;
    return true;
}

bson_type_t mc_FLE2UnindexedEncryptedValue_get_original_bson_type(const mc_FLE2UnindexedEncryptedValue_t *uev,
                                                                  mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(uev);

    if (!uev->parsed) {
        CLIENT_ERR("mc_FLE2UnindexedEncryptedValue_get_original_bson_type must be "
                   "called after mc_FLE2UnindexedEncryptedValue_parse");
        return 0;
    }
    return uev->original_bson_type;
}

const _mongocrypt_buffer_t *mc_FLE2UnindexedEncryptedValue_get_key_uuid(const mc_FLE2UnindexedEncryptedValue_t *uev,
                                                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(uev);

    if (!uev->parsed) {
        CLIENT_ERR("mc_FLE2UnindexedEncryptedValue_get_key_uuid must be "
                   "called after mc_FLE2UnindexedEncryptedValue_parse");
        return NULL;
    }
    return &uev->key_uuid;
}

const _mongocrypt_buffer_t *mc_FLE2UnindexedEncryptedValue_decrypt(_mongocrypt_crypto_t *crypto,
                                                                   mc_FLE2UnindexedEncryptedValue_t *uev,
                                                                   const _mongocrypt_buffer_t *key,
                                                                   mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(uev);
    BSON_ASSERT_PARAM(key);

    if (!uev->parsed) {
        CLIENT_ERR("mc_FLE2UnindexedEncryptedValue_decrypt must be "
                   "called after mc_FLE2UnindexedEncryptedValue_parse");
        return NULL;
    }

    return _mc_FLE2UnindexedEncryptedValueCommon_decrypt(crypto,
                                                         MC_SUBTYPE_FLE2UnindexedEncryptedValue,
                                                         &uev->key_uuid,
                                                         uev->original_bson_type,
                                                         &uev->ciphertext,
                                                         key,
                                                         &uev->plaintext,
                                                         status);
}

bool mc_FLE2UnindexedEncryptedValue_encrypt(_mongocrypt_crypto_t *crypto,
                                            const _mongocrypt_buffer_t *key_uuid,
                                            bson_type_t original_bson_type,
                                            const _mongocrypt_buffer_t *plaintext,
                                            const _mongocrypt_buffer_t *key,
                                            _mongocrypt_buffer_t *out,
                                            mongocrypt_status_t *status) {
    return _mc_FLE2UnindexedEncryptedValueCommon_encrypt(crypto,
                                                         MC_SUBTYPE_FLE2UnindexedEncryptedValue,
                                                         key_uuid,
                                                         original_bson_type,
                                                         plaintext,
                                                         key,
                                                         out,
                                                         status);
}

void mc_FLE2UnindexedEncryptedValue_destroy(mc_FLE2UnindexedEncryptedValue_t *uev) {
    if (NULL == uev) {
        return;
    }
    _mongocrypt_buffer_cleanup(&uev->key_uuid);
    _mongocrypt_buffer_cleanup(&uev->ciphertext);
    _mongocrypt_buffer_cleanup(&uev->plaintext);

    bson_free(uev);
}
