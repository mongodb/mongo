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

#ifndef MONGOCRYPT_INDEXED_ENCRYPTED_VALUE_PRIVATE_H
#define MONGOCRYPT_INDEXED_ENCRYPTED_VALUE_PRIVATE_H

#include "mc-tokens-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-status-private.h"

/**
 * FLE2IndexedEncryptedValue represents an FLE2 encrypted value. It is
 * created server side.
 *
 * FLE2IndexedEncryptedValue represents one of the following payloads:
 * - FLE2IndexedEqualityEncryptedValue
 * - FLE2IndexedRangeEncryptedValue
 *
 * Both payloads share a common prefix. libmongocrypt does not need to parse the
 * edges in FLE2IndexedRangeEncryptedValue.
 */

/* clang-format off */
/*
 * FLE2IndexedEqualityEncryptedValue has the following data layout:
 *
 * struct {
 *   uint8_t fle_blob_subtype = 7;
 *   uint8_t S_KeyId[16];
 *   uint8_t original_bson_type;
 *   uint8_t InnerEncrypted[InnerEncrypted_length];
 * } FLE2IndexedEqualityEncryptedValue
 *
 * InnerEncrypted is the output of: Encrypt(key=ServerDataLevel1Token, plaintext=Inner)
 * ServerDataLevel1Token is created from the key identified by S_KeyId.
 *
 * struct {
 *   uint64_t length; // sizeof(K_KeyId) + ClientEncryptedValue_length;
 *   uint8_t K_KeyId[16];
 *   uint8_t ClientEncryptedValue[ClientEncryptedValue_length];
 *   uint64_t counter;
 *   uint8_t edc[32]; // EDCDerivedFromDataTokenAndContentionFactorToken
 *   uint8_t esc[32]; // ESCDerivedFromDataTokenAndContentionFactorToken
 *   uint8_t ecc[32]; // ECCDerivedFromDataTokenAndContentionFactorToken
 *} Inner
 *
 * ClientEncryptedValue is the output of: EncryptAEAD(key=K_Key, plaintext=ClientValue, associated_data=K_KeyId)
 * K_Key is the key identified by K_KeyId.
 *
 * See https://github.com/mongodb/mongo/blob/fa94f5fb6216a1cc1e23f5ad4df05295b380070e/src/mongo/crypto/fle_crypto.h#L897
 * for the server representation of FLE2IndexedEqualityEncryptedPayload.
 */

/*
 * FLE2IndexedRangeEncryptedPayload shares the data layout with
 * FLE2IndexedEqualityEncryptedValue with the following additional data appended to Inner:
 *
 * uint32_t edgeCount;
 * struct {
 *    uint64_t counter;
 *    uint8_t[32] edc;  // EDCDerivedFromDataTokenAndContentionFactorToken
 *    uint8_t[32] esc;  // ESCDerivedFromDataTokenAndContentionFactorToken
 *    uint8_t[32] ecc;  // ECCDerivedFromDataTokenAndContentionFactorToken
 * } edges[edgeCount];
 *
 * libmongocrypt ignores the edges.
 *
 * See https://github.com/mongodb/mongo/blob/fa94f5fb6216a1cc1e23f5ad4df05295b380070e/src/mongo/crypto/fle_crypto.h#L897
 * for the server representation of FLE2IndexedEqualityEncryptedPayload.
 */
/* clang-format on */

typedef struct _mc_FLE2IndexedEqualityEncryptedValue_t mc_FLE2IndexedEncryptedValue_t;

struct _mc_FLE2IndexedEqualityEncryptedValueTokens {
    uint64_t counter;
    _mongocrypt_buffer_t edc;
    _mongocrypt_buffer_t esc;
    _mongocrypt_buffer_t ecc;
};

typedef struct _mc_FLE2IndexedEqualityEncryptedValueTokens mc_FLE2IndexedEqualityEncryptedValueTokens;

mc_FLE2IndexedEncryptedValue_t *mc_FLE2IndexedEncryptedValue_new(void);

mc_FLE2IndexedEqualityEncryptedValueTokens *mc_FLE2IndexedEqualityEncryptedValueTokens_new(void);

/**
 * This function is used by the server codebase.
 */
bool mc_FLE2IndexedEqualityEncryptedValueTokens_init_from_buffer(mc_FLE2IndexedEqualityEncryptedValueTokens *tokens,
                                                                 _mongocrypt_buffer_t *buf,
                                                                 mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValue_parse(mc_FLE2IndexedEncryptedValue_t *iev,
                                        const _mongocrypt_buffer_t *buf,
                                        mongocrypt_status_t *status);

/**
 * This function is used by the server codebase.
 */
bool mc_FLE2IndexedEncryptedValue_write(_mongocrypt_crypto_t *crypto,
                                        const bson_type_t original_bson_type,
                                        const _mongocrypt_buffer_t *S_KeyId,
                                        const _mongocrypt_buffer_t *ClientEncryptedValue,
                                        mc_ServerDataEncryptionLevel1Token_t *token,
                                        mc_FLE2IndexedEqualityEncryptedValueTokens *index_tokens,
                                        _mongocrypt_buffer_t *buf,
                                        mongocrypt_status_t *status);

/* mc_FLE2IndexedEncryptedValue_get_original_bson_type returns
 * original_bson_type. Returns 0 and sets @status on error.
 * It is an error to call before mc_FLE2IndexedEncryptedValue_parse. */
bson_type_t mc_FLE2IndexedEncryptedValue_get_original_bson_type(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                mongocrypt_status_t *status);

/* mc_FLE2IndexedEncryptedValue_get_S_KeyId returns S_KeyId. Returns
 * NULL and sets @status on error. It is an error to call before
 * mc_FLE2IndexedEncryptedValue_parse. */
const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValue_get_S_KeyId(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                     mongocrypt_status_t *status);

/* mc_FLE2IndexedEncryptedValue_add_S_Key decrypts InnerEncrypted.
 * Returns false and sets @status on error. It is an error to call before
 * mc_FLE2IndexedEncryptedValue_parse. */
bool mc_FLE2IndexedEncryptedValue_add_S_Key(_mongocrypt_crypto_t *crypto,
                                            mc_FLE2IndexedEncryptedValue_t *iev,
                                            const _mongocrypt_buffer_t *S_Key,
                                            mongocrypt_status_t *status);

/* mc_FLE2IndexedEncryptedValue_decrypt decrypts InnerEncrypted with the
 * ServerDataEncryptionLevel1Token on the server-side. Returns false and sets
 * @status on error. It is an error to call before
 * mc_FLE2IndexedEncryptedValue_parse.
 *
 * This function is used by the server codebase.
 */
bool mc_FLE2IndexedEncryptedValue_decrypt_equality(_mongocrypt_crypto_t *crypto,
                                                   mc_FLE2IndexedEncryptedValue_t *iev,
                                                   mc_ServerDataEncryptionLevel1Token_t *token,
                                                   mc_FLE2IndexedEqualityEncryptedValueTokens *indexed_tokens,
                                                   mongocrypt_status_t *status);

/* mc_FLE2IndexedEncryptedValue_get_K_KeyId returns Inner.K_KeyId.
 * Returns NULL and sets @status on error. It is an error to call before
 * mc_FLE2IndexedEncryptedValue_add_S_Key. */
const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValue_get_K_KeyId(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                     mongocrypt_status_t *status);

/* mc_FLE2IndexedEncryptedValue_add_K_Key decrypts
 * Inner.ClientEncryptedValue. Returns false and sets @status on error. Must
 * not be called before mc_FLE2IndexedEncryptedValue_add_S_Key. */
bool mc_FLE2IndexedEncryptedValue_add_K_Key(_mongocrypt_crypto_t *crypto,
                                            mc_FLE2IndexedEncryptedValue_t *iev,
                                            const _mongocrypt_buffer_t *K_Key,
                                            mongocrypt_status_t *status);

/* mc_FLE2IndexedEncryptedValue_get_ClientValue returns the decrypted
 * Inner.ClientEncryptedValue. Returns NULL and sets @status on error.
 * It is an error to call before mc_FLE2IndexedEncryptedValue_add_K_Key.
 */
const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValue_get_ClientValue(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                         mongocrypt_status_t *status);

/**
 * This function is used by the server codebase.
 */
const _mongocrypt_buffer_t *
mc_FLE2IndexedEncryptedValue_get_ClientEncryptedValue(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                      mongocrypt_status_t *status);

void mc_FLE2IndexedEncryptedValue_destroy(mc_FLE2IndexedEncryptedValue_t *iev);

void mc_FLE2IndexedEqualityEncryptedValueTokens_destroy(mc_FLE2IndexedEqualityEncryptedValueTokens *tokens);

#endif /* MONGOCRYPT_INDEXED_ENCRYPTED_VALUE_PRIVATE_H */
