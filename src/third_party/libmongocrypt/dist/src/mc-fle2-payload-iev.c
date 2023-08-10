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

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-private.h"

#include "mc-fle-blob-subtype-private.h"
#include "mc-fle2-payload-iev-private.h"
#include "mc-reader-private.h"
#include "mc-tokens-private.h"
#include "mc-writer-private.h"
#include <stdint.h>

#define CHECK_AND_RETURN(x)                                                                                            \
    if (!(x)) {                                                                                                        \
        return false;                                                                                                  \
    }

struct _mc_FLE2IndexedEqualityEncryptedValue_t {
    _mongocrypt_buffer_t S_KeyId;
    _mongocrypt_buffer_t InnerEncrypted;
    _mongocrypt_buffer_t Inner;
    _mongocrypt_buffer_t K_KeyId;
    _mongocrypt_buffer_t ClientValue;
    _mongocrypt_buffer_t ClientEncryptedValue;
    uint8_t original_bson_type;
    uint8_t fle_blob_subtype;
    bool parsed;
    bool inner_decrypted;
    bool client_value_decrypted;
};

mc_FLE2IndexedEncryptedValue_t *mc_FLE2IndexedEncryptedValue_new(void) {
    return bson_malloc0(sizeof(mc_FLE2IndexedEncryptedValue_t));
}

mc_FLE2IndexedEqualityEncryptedValueTokens *mc_FLE2IndexedEqualityEncryptedValueTokens_new(void) {
    return bson_malloc0(sizeof(mc_FLE2IndexedEqualityEncryptedValueTokens));
}

bool mc_FLE2IndexedEqualityEncryptedValueTokens_init_from_buffer(mc_FLE2IndexedEqualityEncryptedValueTokens *tokens,
                                                                 _mongocrypt_buffer_t *buf,
                                                                 mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(tokens);
    BSON_ASSERT_PARAM(buf);

    mc_reader_t reader;
    mc_reader_init_from_buffer(&reader, buf, __FUNCTION__);

    CHECK_AND_RETURN(mc_reader_read_u64(&reader, &tokens->counter, status));

    CHECK_AND_RETURN(mc_reader_read_prfblock_buffer(&reader, &tokens->edc, status));

    CHECK_AND_RETURN(mc_reader_read_prfblock_buffer(&reader, &tokens->esc, status));

    CHECK_AND_RETURN(mc_reader_read_prfblock_buffer(&reader, &tokens->ecc, status));

    return true;
}

static bool mc_fle2IndexedEncryptedValue_encrypt(_mongocrypt_crypto_t *crypto,
                                                 const _mongocrypt_buffer_t *ClientEncryptedValue,
                                                 mc_ServerDataEncryptionLevel1Token_t *token,
                                                 mc_FLE2IndexedEqualityEncryptedValueTokens *index_tokens,
                                                 _mongocrypt_buffer_t *out,
                                                 mongocrypt_status_t *status);

bool safe_uint32_t_sum(const uint32_t a, const uint32_t b, uint32_t *out, mongocrypt_status_t *status) {
    if (a > UINT32_MAX - b) {
        CLIENT_ERR("safe_uint32_t_sum overflow, %" PRIu32 ", %" PRIu32, a, b);
        return false;
    }
    *out = a + b;
    return true;
}

bool mc_FLE2IndexedEncryptedValue_write(_mongocrypt_crypto_t *crypto,
                                        const bson_type_t original_bson_type,
                                        const _mongocrypt_buffer_t *S_KeyId,
                                        const _mongocrypt_buffer_t *ClientEncryptedValue,
                                        mc_ServerDataEncryptionLevel1Token_t *token,
                                        mc_FLE2IndexedEqualityEncryptedValueTokens *index_tokens,
                                        _mongocrypt_buffer_t *buf,
                                        mongocrypt_status_t *status) {
#define CHECK_AND_GOTO(x)                                                                                              \
    if (!(x)) {                                                                                                        \
        goto cleanup;                                                                                                  \
    }

    const _mongocrypt_value_encryption_algorithm_t *fle2alg = _mcFLE2Algorithm();
    bool ok = false;

    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(index_tokens);
    BSON_ASSERT_PARAM(S_KeyId);
    BSON_ASSERT_PARAM(ClientEncryptedValue);
    BSON_ASSERT_PARAM(token);
    BSON_ASSERT_PARAM(index_tokens);
    BSON_ASSERT_PARAM(buf);

    if (ClientEncryptedValue->len == 0) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_write iev must have an encrypted value");
        return ok;
    }

    if (S_KeyId->len == 0) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_write iev SKeyId must have value");
        return ok;
    }

    _mongocrypt_buffer_t encryption_out;
    _mongocrypt_buffer_init(&encryption_out);

    CHECK_AND_GOTO(mc_fle2IndexedEncryptedValue_encrypt(crypto,
                                                        ClientEncryptedValue,
                                                        token,
                                                        index_tokens,
                                                        &encryption_out,
                                                        status));
    uint32_t expected_plaintext_size = 0;
    CHECK_AND_GOTO(safe_uint32_t_sum(ClientEncryptedValue->len,
                                     (uint32_t)(sizeof(uint64_t) * 2 + sizeof(uint32_t) * 3),
                                     &expected_plaintext_size,
                                     status));

    uint32_t expected_cipher_size = fle2alg->get_ciphertext_len(expected_plaintext_size, status);

    if (expected_cipher_size == 0) {
        CHECK_AND_GOTO(false);
    }

    uint32_t expected_buf_size = 0;
    CHECK_AND_RETURN(
        safe_uint32_t_sum(expected_cipher_size, (uint32_t)(1 + sizeof(S_KeyId)), &expected_buf_size, status));

    if (buf->len < expected_buf_size) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_write buf is not large enough for iev");
        CHECK_AND_GOTO(false);
    }

    mc_writer_t writer;
    mc_writer_init_from_buffer(&writer, buf, __FUNCTION__);

    const uint8_t subtype = (uint8_t)MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue;

    if (((int)original_bson_type < 0) || ((int)original_bson_type > 0xFF)) {
        CLIENT_ERR("Field 't' must be a valid BSON type, got: %d", original_bson_type);
        CHECK_AND_GOTO(false);
    }

    const uint8_t bson_type = (uint8_t)original_bson_type;

    CHECK_AND_GOTO(mc_writer_write_u8(&writer, subtype, status));
    CHECK_AND_GOTO(mc_writer_write_buffer(&writer, S_KeyId, S_KeyId->len, status));
    CHECK_AND_GOTO(mc_writer_write_u8(&writer, bson_type, status));

    CHECK_AND_GOTO(mc_writer_write_buffer(&writer, &encryption_out, encryption_out.len, status));

    ok = true;

cleanup:
    _mongocrypt_buffer_cleanup(&encryption_out);
    return ok;
#undef CHECK_AND_GOTO
}

static bool mc_fle2IndexedEncryptedValue_encrypt(_mongocrypt_crypto_t *crypto,
                                                 const _mongocrypt_buffer_t *ClientEncryptedValue,
                                                 mc_ServerDataEncryptionLevel1Token_t *token,
                                                 mc_FLE2IndexedEqualityEncryptedValueTokens *index_tokens,
                                                 _mongocrypt_buffer_t *out,
                                                 mongocrypt_status_t *status) {
#define CHECK_AND_GOTO(x)                                                                                              \
    if (!(x)) {                                                                                                        \
        goto cleanup;                                                                                                  \
    }

    const _mongocrypt_value_encryption_algorithm_t *fle2alg = _mcFLE2Algorithm();
    bool ok = false;
    _mongocrypt_buffer_t in;
    _mongocrypt_buffer_t iv;

    _mongocrypt_buffer_init(&in);
    _mongocrypt_buffer_init_size(&iv, MONGOCRYPT_IV_LEN);

    uint32_t expected_buf_size = 0;
    CHECK_AND_GOTO(safe_uint32_t_sum(ClientEncryptedValue->len,
                                     (uint32_t)(sizeof(uint64_t) * 2 + (32 * 3)),
                                     &expected_buf_size,
                                     status));

    _mongocrypt_buffer_resize(&in, expected_buf_size);

    uint32_t ciphertext_len = fle2alg->get_ciphertext_len(expected_buf_size, status);

    if (ciphertext_len == 0) {
        return false;
    }

    _mongocrypt_buffer_resize(out, ciphertext_len);

    mc_writer_t writer;
    mc_writer_init_from_buffer(&writer, &in, __FUNCTION__);

    uint64_t length;
    length = ClientEncryptedValue->len;
    CHECK_AND_GOTO(mc_writer_write_u64(&writer, length, status));

    CHECK_AND_GOTO(mc_writer_write_buffer(&writer, ClientEncryptedValue, ClientEncryptedValue->len, status));

    CHECK_AND_GOTO(mc_writer_write_u64(&writer, index_tokens->counter, status));

    CHECK_AND_GOTO(mc_writer_write_prfblock_buffer(&writer, &index_tokens->edc, status));

    CHECK_AND_GOTO(mc_writer_write_prfblock_buffer(&writer, &index_tokens->esc, status));

    CHECK_AND_GOTO(mc_writer_write_prfblock_buffer(&writer, &index_tokens->ecc, status));

    const _mongocrypt_buffer_t *token_buf = mc_ServerDataEncryptionLevel1Token_get(token);

    uint32_t bytes_written;

    CHECK_AND_GOTO(_mongocrypt_random(crypto, &iv, MONGOCRYPT_IV_LEN, status));

    CHECK_AND_GOTO(fle2alg->do_encrypt(crypto, &iv, NULL /* aad */, token_buf, &in, out, &bytes_written, status));

    ok = true;

cleanup:
    _mongocrypt_buffer_cleanup(&iv);
    _mongocrypt_buffer_cleanup(&in);
    return ok;
#undef CHECK_AND_GOTO
}

bool mc_FLE2IndexedEncryptedValue_parse(mc_FLE2IndexedEncryptedValue_t *iev,
                                        const _mongocrypt_buffer_t *buf,
                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(buf);

    if (iev->parsed) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_parse must not be called twice");
        return false;
    }

    mc_reader_t reader;
    mc_reader_init_from_buffer(&reader, buf, __FUNCTION__);

    CHECK_AND_RETURN(mc_reader_read_u8(&reader, &iev->fle_blob_subtype, status));

    if (iev->fle_blob_subtype != MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue
        && iev->fle_blob_subtype != MC_SUBTYPE_FLE2IndexedRangeEncryptedValue) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_parse expected "
                   "fle_blob_subtype %d or %d got: %" PRIu8,
                   MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue,
                   MC_SUBTYPE_FLE2IndexedRangeEncryptedValue,
                   iev->fle_blob_subtype);
        return false;
    }

    /* Read S_KeyId. */
    CHECK_AND_RETURN(mc_reader_read_uuid_buffer(&reader, &iev->S_KeyId, status));

    /* Read original_bson_type. */
    CHECK_AND_RETURN(mc_reader_read_u8(&reader, &iev->original_bson_type, status));

    /* Read InnerEncrypted. */
    CHECK_AND_RETURN(mc_reader_read_buffer_to_end(&reader, &iev->InnerEncrypted, status));

    iev->parsed = true;
    return true;
}

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValue_get_S_KeyId(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                     mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (!iev->parsed) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_get_S_KeyId must be "
                   "called after mc_FLE2IndexedEncryptedValue_parse");
        return NULL;
    }

    return &iev->S_KeyId;
}

static bool mc_FLE2IndexedEncryptedValue_decrypt(_mongocrypt_crypto_t *crypto,
                                                 mc_FLE2IndexedEncryptedValue_t *iev,
                                                 mc_ServerDataEncryptionLevel1Token_t *token,
                                                 mc_FLE2IndexedEqualityEncryptedValueTokens *indexed_tokens,
                                                 mongocrypt_status_t *status);

bool mc_FLE2IndexedEncryptedValue_add_S_Key(_mongocrypt_crypto_t *crypto,
                                            mc_FLE2IndexedEncryptedValue_t *iev,
                                            const _mongocrypt_buffer_t *S_Key,
                                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(S_Key);

    /* Attempt to decrypt InnerEncrypted */
    if (S_Key->len != MONGOCRYPT_KEY_LEN) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_add_S_Key expected "
                   "S_Key to be %d bytes, got: %" PRIu32,
                   MONGOCRYPT_KEY_LEN,
                   S_Key->len);
        return false;
    }

    /* Get the TokenKey from the last 32 bytes of S_Key */
    _mongocrypt_buffer_t TokenKey;
    if (!_mongocrypt_buffer_from_subrange(&TokenKey,
                                          S_Key,
                                          S_Key->len - MONGOCRYPT_TOKEN_KEY_LEN,
                                          MONGOCRYPT_TOKEN_KEY_LEN)) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_add_S_Key unable to "
                   "parse TokenKey from S_Key");
        return false;
    }

    /* Use TokenKey to create ServerDataEncryptionLevel1Token and decrypt
     * InnerEncrypted. */

    mc_ServerDataEncryptionLevel1Token_t *token = mc_ServerDataEncryptionLevel1Token_new(crypto, &TokenKey, status);
    if (!token) {
        return false;
    }

    bool ret = mc_FLE2IndexedEncryptedValue_decrypt(crypto, iev, token, NULL, status);

    mc_ServerDataEncryptionLevel1Token_destroy(token);

    return ret;
}

static bool mc_FLE2IndexedEncryptedValue_decrypt(_mongocrypt_crypto_t *crypto,
                                                 mc_FLE2IndexedEncryptedValue_t *iev,
                                                 mc_ServerDataEncryptionLevel1Token_t *token,
                                                 mc_FLE2IndexedEqualityEncryptedValueTokens *indexed_tokens,
                                                 mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle2alg = _mcFLE2Algorithm();
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(token);

    if (!iev->parsed) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_decrypt must be "
                   "called after mc_FLE2IndexedEncryptedValue_parse");
        return false;
    }

    if (iev->inner_decrypted) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_decrypt must not be "
                   "called twice");
        return false;
    }

    const _mongocrypt_buffer_t *token_buf = mc_ServerDataEncryptionLevel1Token_get(token);
    uint32_t bytes_written;

    _mongocrypt_buffer_resize(&iev->Inner, fle2alg->get_plaintext_len(iev->InnerEncrypted.len, status));

    /* Decrypt InnerEncrypted. */
    if (!fle2alg->do_decrypt(crypto,
                             NULL /* aad */,
                             token_buf,
                             &iev->InnerEncrypted,
                             &iev->Inner,
                             &bytes_written,
                             status)) {
        return false;
    }

    mc_reader_t reader;
    mc_reader_init_from_buffer(&reader, &iev->Inner, __FUNCTION__);

    /* Parse Inner for K_KeyId. */
    uint64_t length; /* length is sizeof(K_KeyId) + ClientEncryptedValue_length. */
    CHECK_AND_RETURN(mc_reader_read_u64(&reader, &length, status));

    /* Read K_KeyId. */
    CHECK_AND_RETURN(mc_reader_read_uuid_buffer(&reader, &iev->K_KeyId, status));

    /* Read ClientEncryptedValue. */
    uint64_t expected_length = mc_reader_get_consumed_length(&reader) + length - 16;
    if (length > iev->Inner.len || expected_length > iev->Inner.len) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_decrypt expected "
                   "byte length >= %" PRIu64 " got: %" PRIu32,
                   expected_length,
                   iev->Inner.len);
        return false;
    }

    CHECK_AND_RETURN(mc_reader_read_buffer(&reader, &iev->ClientEncryptedValue, length - 16, status));

    // Caller has asked us to parse the other tokens
    if (indexed_tokens != NULL) {
        CHECK_AND_RETURN(mc_reader_read_u64(&reader, &indexed_tokens->counter, status));

        CHECK_AND_RETURN(mc_reader_read_prfblock_buffer(&reader, &indexed_tokens->edc, status));

        CHECK_AND_RETURN(mc_reader_read_prfblock_buffer(&reader, &indexed_tokens->esc, status));

        CHECK_AND_RETURN(mc_reader_read_prfblock_buffer(&reader, &indexed_tokens->ecc, status));
    }

    iev->inner_decrypted = true;
    return true;
}

bool mc_FLE2IndexedEncryptedValue_decrypt_equality(_mongocrypt_crypto_t *crypto,
                                                   mc_FLE2IndexedEncryptedValue_t *iev,
                                                   mc_ServerDataEncryptionLevel1Token_t *token,
                                                   mc_FLE2IndexedEqualityEncryptedValueTokens *indexed_tokens,
                                                   mongocrypt_status_t *status) {
    BSON_ASSERT(iev->fle_blob_subtype == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue);

    return mc_FLE2IndexedEncryptedValue_decrypt(crypto, iev, token, indexed_tokens, status);
}

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValue_get_K_KeyId(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                     mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (!iev->inner_decrypted) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_get_K_KeyId must be called "
                   "after mc_FLE2IndexedEncryptedValue_add_S_Key");
        return NULL;
    }
    return &iev->K_KeyId;
}

bool mc_FLE2IndexedEncryptedValue_add_K_Key(_mongocrypt_crypto_t *crypto,
                                            mc_FLE2IndexedEncryptedValue_t *iev,
                                            const _mongocrypt_buffer_t *K_Key,
                                            mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle2aead = _mcFLE2AEADAlgorithm();
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(iev);
    BSON_ASSERT_PARAM(K_Key);

    if (!iev->inner_decrypted) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_add_K_Key must be called after "
                   "mc_FLE2IndexedEncryptedValue_add_S_Key");
        return false;
    }
    if (iev->client_value_decrypted) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_add_K_Key must not be "
                   "called twice");
        return false;
    }
    /* Attempt to decrypt ClientEncryptedValue */
    _mongocrypt_buffer_resize(&iev->ClientValue, fle2aead->get_plaintext_len(iev->ClientEncryptedValue.len, status));
    uint32_t bytes_written;
    if (!fle2aead->do_decrypt(crypto,
                              &iev->K_KeyId,
                              K_Key,
                              &iev->ClientEncryptedValue,
                              &iev->ClientValue,
                              &bytes_written,
                              status)) {
        return false;
    }
    iev->client_value_decrypted = true;
    return true;
}

const _mongocrypt_buffer_t *
mc_FLE2IndexedEncryptedValue_get_ClientEncryptedValue(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                      mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    return &iev->ClientEncryptedValue;
}

const _mongocrypt_buffer_t *mc_FLE2IndexedEncryptedValue_get_ClientValue(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (!iev->client_value_decrypted) {
        CLIENT_ERR("mc_FLE2IndexedEqualityEncryptedValue_getClientValue must be called "
                   "after mc_FLE2IndexedEncryptedValue_add_K_Key");
        return NULL;
    }
    return &iev->ClientValue;
}

void mc_FLE2IndexedEncryptedValue_destroy(mc_FLE2IndexedEncryptedValue_t *iev) {
    if (!iev) {
        return;
    }
    _mongocrypt_buffer_cleanup(&iev->S_KeyId);
    _mongocrypt_buffer_cleanup(&iev->InnerEncrypted);
    _mongocrypt_buffer_cleanup(&iev->Inner);
    _mongocrypt_buffer_cleanup(&iev->K_KeyId);
    _mongocrypt_buffer_cleanup(&iev->ClientValue);
    _mongocrypt_buffer_cleanup(&iev->ClientEncryptedValue);
    bson_free(iev);
}

void mc_FLE2IndexedEqualityEncryptedValueTokens_destroy(mc_FLE2IndexedEqualityEncryptedValueTokens *tokens) {
    if (!tokens) {
        return;
    }

    _mongocrypt_buffer_cleanup(&tokens->edc);
    _mongocrypt_buffer_cleanup(&tokens->esc);
    _mongocrypt_buffer_cleanup(&tokens->ecc);
    bson_free(tokens);
}

bson_type_t mc_FLE2IndexedEncryptedValue_get_original_bson_type(const mc_FLE2IndexedEncryptedValue_t *iev,
                                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(iev);

    if (!iev->parsed) {
        CLIENT_ERR("mc_FLE2IndexedEncryptedValue_get_original_bson_type must be "
                   "called after mc_FLE2IndexedEncryptedValue_parse");
        return 0;
    }
    return iev->original_bson_type;
}
