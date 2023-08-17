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

#include "mc-fle-blob-subtype-private.h"
#include "mc-fle2-encryption-placeholder-private.h"
#include "mc-fle2-find-equality-payload-private-v2.h"
#include "mc-fle2-find-equality-payload-private.h"
#include "mc-fle2-find-range-payload-private-v2.h"
#include "mc-fle2-find-range-payload-private.h"
#include "mc-fle2-insert-update-payload-private-v2.h"
#include "mc-fle2-insert-update-payload-private.h"
#include "mc-fle2-payload-uev-private.h"
#include "mc-fle2-payload-uev-v2-private.h"
#include "mc-range-edge-generation-private.h"
#include "mc-range-encoding-private.h"
#include "mc-range-mincover-private.h"
#include "mc-tokens-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-ciphertext-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-key-broker-private.h"
#include "mongocrypt-marking-private.h"
#include "mongocrypt-util-private.h" // mc_bson_type_to_string
#include "mongocrypt.h"

#include <math.h> // isinf

static bool
_mongocrypt_marking_parse_fle1_placeholder(const bson_t *in, _mongocrypt_marking_t *out, mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_ki = false, has_ka = false, has_a = false, has_v = false;

    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    out->type = MONGOCRYPT_MARKING_FLE1_BY_ID;

    if (!bson_iter_init(&iter, in)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field;

        field = bson_iter_key(&iter);
        BSON_ASSERT(field);
        if (0 == strcmp("ki", field)) {
            has_ki = true;
            if (!_mongocrypt_buffer_from_uuid_iter(&out->key_id, &iter)) {
                CLIENT_ERR("key id must be a UUID");
                return false;
            }
            continue;
        }

        if (0 == strcmp("ka", field)) {
            has_ka = true;
            /* Some bson_value types are not allowed to be key alt names */
            const bson_value_t *value;

            value = bson_iter_value(&iter);

            if (!BSON_ITER_HOLDS_UTF8(&iter)) {
                CLIENT_ERR("key alt name must be a UTF8");
                return false;
            }
            /* CDRIVER-3100 We must make a copy of this value; the result of
             * bson_iter_value is ephemeral. */
            bson_value_copy(value, &out->key_alt_name);
            out->type = MONGOCRYPT_MARKING_FLE1_BY_ALTNAME;
            continue;
        }

        if (0 == strcmp("v", field)) {
            has_v = true;
            memcpy(&out->v_iter, &iter, sizeof(bson_iter_t));
            continue;
        }

        if (0 == strcmp("a", field)) {
            int32_t algorithm;

            has_a = true;
            if (!BSON_ITER_HOLDS_INT32(&iter)) {
                CLIENT_ERR("invalid marking, 'a' must be an int32");
                return false;
            }
            algorithm = bson_iter_int32(&iter);
            if (algorithm != MONGOCRYPT_ENCRYPTION_ALGORITHM_DETERMINISTIC
                && algorithm != MONGOCRYPT_ENCRYPTION_ALGORITHM_RANDOM) {
                CLIENT_ERR("invalid algorithm value: %d", algorithm);
                return false;
            }
            out->algorithm = (mongocrypt_encryption_algorithm_t)algorithm;
            continue;
        }

        CLIENT_ERR("unrecognized field '%s'", field);
        return false;
    }

    if (!has_v) {
        CLIENT_ERR("no 'v' specified");
        return false;
    }

    if (!has_ki && !has_ka) {
        CLIENT_ERR("neither 'ki' nor 'ka' specified");
        return false;
    }

    if (has_ki && has_ka) {
        CLIENT_ERR("both 'ki' and 'ka' specified");
        return false;
    }

    if (!has_a) {
        CLIENT_ERR("no 'a' specified");
        return false;
    }

    return true;
}

static bool
_mongocrypt_marking_parse_fle2_placeholder(const bson_t *in, _mongocrypt_marking_t *out, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    out->type = MONGOCRYPT_MARKING_FLE2_ENCRYPTION;
    return mc_FLE2EncryptionPlaceholder_parse(&out->fle2, in, status);
}

bool _mongocrypt_marking_parse_unowned(const _mongocrypt_buffer_t *in,
                                       _mongocrypt_marking_t *out,
                                       mongocrypt_status_t *status) {
    bson_t bson;

    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    _mongocrypt_marking_init(out);
    /* 5 for minimal BSON object, plus one for blob subtype */
    if (in->len < 6) {
        CLIENT_ERR("invalid marking, length < 6");
        return false;
    }

    if (!bson_init_static(&bson, in->data + 1, in->len - 1) || !bson_validate(&bson, BSON_VALIDATE_NONE, NULL)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    if (in->data[0] == MC_SUBTYPE_FLE1EncryptionPlaceholder) {
        return _mongocrypt_marking_parse_fle1_placeholder(&bson, out, status);
    } else if (in->data[0] == MC_SUBTYPE_FLE2EncryptionPlaceholder) {
        return _mongocrypt_marking_parse_fle2_placeholder(&bson, out, status);
    } else {
        CLIENT_ERR("invalid marking, first byte must be 0 or 3");
        return false;
    }
}

void _mongocrypt_marking_init(_mongocrypt_marking_t *marking) {
    BSON_ASSERT_PARAM(marking);

    memset(marking, 0, sizeof(*marking));
}

void _mongocrypt_marking_cleanup(_mongocrypt_marking_t *marking) {
    if (!marking) {
        return;
    }
    if (marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION) {
        mc_FLE2EncryptionPlaceholder_cleanup(&marking->fle2);
        return;
    }

    // else FLE1
    _mongocrypt_buffer_cleanup(&marking->key_id);
    bson_value_destroy(&marking->key_alt_name);
}

/**
 * Calculates:
 * E?CToken = HMAC(collectionLevel1Token, n)
 * E?CDerivedFromDataToken = HMAC(E?CToken, value)
 * E?CDerivedFromDataTokenAndCounter = HMAC(E?CDerivedFromDataToken, c)
 *
 * E?C = EDC|ESC|ECC
 * n = 1 for EDC, 2 for ESC, 3 for ECC
 * c = maxContentionCounter
 *
 * E?CDerivedFromDataTokenAndCounter is saved to out,
 * which is initialized even on failure.
 */
#define DERIVE_TOKEN_IMPL(Name)                                                                                        \
    static bool _fle2_derive_##Name##_token(_mongocrypt_crypto_t *crypto,                                              \
                                            _mongocrypt_buffer_t *out,                                                 \
                                            const mc_CollectionsLevel1Token_t *level1Token,                            \
                                            const _mongocrypt_buffer_t *value,                                         \
                                            bool useCounter,                                                           \
                                            int64_t counter,                                                           \
                                            mongocrypt_status_t *status) {                                             \
        BSON_ASSERT_PARAM(crypto);                                                                                     \
        BSON_ASSERT_PARAM(out);                                                                                        \
        BSON_ASSERT_PARAM(level1Token);                                                                                \
        BSON_ASSERT_PARAM(value);                                                                                      \
                                                                                                                       \
        _mongocrypt_buffer_init(out);                                                                                  \
                                                                                                                       \
        mc_##Name##Token_t *token = mc_##Name##Token_new(crypto, level1Token, status);                                 \
        if (!token) {                                                                                                  \
            return false;                                                                                              \
        }                                                                                                              \
                                                                                                                       \
        mc_##Name##DerivedFromDataToken_t *fromDataToken =                                                             \
            mc_##Name##DerivedFromDataToken_new(crypto, token, value, status);                                         \
        mc_##Name##Token_destroy(token);                                                                               \
        if (!fromDataToken) {                                                                                          \
            return false;                                                                                              \
        }                                                                                                              \
                                                                                                                       \
        if (!useCounter) {                                                                                             \
            /* FindEqualityPayload uses *fromDataToken */                                                              \
            _mongocrypt_buffer_copy_to(mc_##Name##DerivedFromDataToken_get(fromDataToken), out);                       \
            mc_##Name##DerivedFromDataToken_destroy(fromDataToken);                                                    \
            return true;                                                                                               \
        }                                                                                                              \
                                                                                                                       \
        BSON_ASSERT(counter >= 0);                                                                                     \
        /* InsertUpdatePayload continues through *fromDataTokenAndCounter */                                           \
        mc_##Name##DerivedFromDataTokenAndCounter_t *fromTokenAndCounter =                                             \
            mc_##Name##DerivedFromDataTokenAndCounter_new(crypto, fromDataToken, (uint64_t)counter, status);           \
        mc_##Name##DerivedFromDataToken_destroy(fromDataToken);                                                        \
        if (!fromTokenAndCounter) {                                                                                    \
            return false;                                                                                              \
        }                                                                                                              \
                                                                                                                       \
        _mongocrypt_buffer_copy_to(mc_##Name##DerivedFromDataTokenAndCounter_get(fromTokenAndCounter), out);           \
        mc_##Name##DerivedFromDataTokenAndCounter_destroy(fromTokenAndCounter);                                        \
                                                                                                                       \
        return true;                                                                                                   \
    }

DERIVE_TOKEN_IMPL(EDC)
DERIVE_TOKEN_IMPL(ESC)
DERIVE_TOKEN_IMPL(ECC)

#undef DERIVE_TOKEN_IMPL

static bool _fle2_derive_serverDerivedFromDataToken(_mongocrypt_crypto_t *crypto,
                                                    _mongocrypt_buffer_t *out,
                                                    const mc_ServerTokenDerivationLevel1Token_t *level1Token,
                                                    const _mongocrypt_buffer_t *value,
                                                    mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(level1Token);
    BSON_ASSERT_PARAM(value);
    BSON_ASSERT_PARAM(status);

    _mongocrypt_buffer_init(out);

    mc_ServerDerivedFromDataToken_t *token = mc_ServerDerivedFromDataToken_new(crypto, level1Token, value, status);
    if (!token) {
        return false;
    }

    _mongocrypt_buffer_copy_to(mc_ServerDerivedFromDataToken_get(token), out);
    mc_ServerDerivedFromDataToken_destroy(token);
    return true;
}

static bool _fle2_placeholder_aes_ctr_encrypt(_mongocrypt_crypto_t *crypto,
                                              const _mongocrypt_buffer_t *key,
                                              const _mongocrypt_buffer_t *in,
                                              _mongocrypt_buffer_t *out,
                                              mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle2alg = _mcFLE2Algorithm();
    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(key);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    _mongocrypt_buffer_t iv;
    const uint32_t cipherlen = fle2alg->get_ciphertext_len(in->len, status);
    if (cipherlen == 0) {
        return false;
    }
    uint32_t written = 0;

    _mongocrypt_buffer_init_size(out, cipherlen);

    BSON_ASSERT(_mongocrypt_buffer_from_subrange(&iv, out, 0, MONGOCRYPT_IV_LEN));
    if (!_mongocrypt_random(crypto, &iv, MONGOCRYPT_IV_LEN, status)) {
        return false;
    }

    if (!fle2alg->do_encrypt(crypto, &iv, NULL /* aad */, key, in, out, &written, status)) {
        _mongocrypt_buffer_cleanup(out);
        _mongocrypt_buffer_init(out);
        return false;
    }

    return true;
}

static bool _fle2_placeholder_aes_aead_encrypt(_mongocrypt_key_broker_t *kb,
                                               const _mongocrypt_value_encryption_algorithm_t *algorithm,
                                               _mongocrypt_buffer_t *out,
                                               const _mongocrypt_buffer_t *keyId,
                                               const _mongocrypt_buffer_t *in,
                                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(keyId);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(kb->crypt);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    _mongocrypt_buffer_t iv, key;
    const uint32_t cipherlen = algorithm->get_ciphertext_len(in->len, status);
    if (cipherlen == 0) {
        return false;
    }
    uint32_t written = 0;
    bool res;

    if (!_mongocrypt_key_broker_decrypted_key_by_id(kb, keyId, &key)) {
        CLIENT_ERR("unable to retrieve key");
        return false;
    }

    _mongocrypt_buffer_init_size(&iv, MONGOCRYPT_IV_LEN);
    if (!_mongocrypt_random(crypto, &iv, iv.len, status)) {
        _mongocrypt_buffer_cleanup(&key);
        return false;
    }

    _mongocrypt_buffer_init_size(out, cipherlen);
    res = algorithm->do_encrypt(crypto, &iv, keyId, &key, in, out, &written, status);
    _mongocrypt_buffer_cleanup(&key);
    _mongocrypt_buffer_cleanup(&iv);

    if (!res) {
        _mongocrypt_buffer_cleanup(out);
        _mongocrypt_buffer_init(out);
        return false;
    }

    return true;
}

// p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndCounter ||
//                            ECCDerivedFromDataTokenAndCounter)
static bool _fle2_derive_encrypted_token(_mongocrypt_crypto_t *crypto,
                                         _mongocrypt_buffer_t *out,
                                         const mc_CollectionsLevel1Token_t *collectionsLevel1Token,
                                         const _mongocrypt_buffer_t *escDerivedToken,
                                         const _mongocrypt_buffer_t *eccDerivedToken,
                                         mongocrypt_status_t *status) {
    mc_ECOCToken_t *ecocToken = mc_ECOCToken_new(crypto, collectionsLevel1Token, status);
    if (!ecocToken) {
        return false;
    }

    _mongocrypt_buffer_t tmp;
    _mongocrypt_buffer_init(&tmp);
    const _mongocrypt_buffer_t *p = &tmp;
    if (!eccDerivedToken) {
        // FLE2v2
        p = escDerivedToken;
    } else {
        // FLE2v1
        const _mongocrypt_buffer_t tokens[] = {*escDerivedToken, *eccDerivedToken};
        _mongocrypt_buffer_concat(&tmp, tokens, 2);
    }

    const bool ok = _fle2_placeholder_aes_ctr_encrypt(crypto, mc_ECOCToken_get(ecocToken), p, out, status);
    _mongocrypt_buffer_cleanup(&tmp);
    mc_ECOCToken_destroy(ecocToken);
    return ok;
}

// Field derivations shared by both INSERT and FIND payloads.
typedef struct {
    _mongocrypt_buffer_t tokenKey;
    mc_CollectionsLevel1Token_t *collectionsLevel1Token;
    mc_ServerDataEncryptionLevel1Token_t *serverDataEncryptionLevel1Token;
    mc_ServerTokenDerivationLevel1Token_t *serverTokenDerivationLevel1Token; // v2
    _mongocrypt_buffer_t edcDerivedToken;
    _mongocrypt_buffer_t escDerivedToken;
    _mongocrypt_buffer_t eccDerivedToken;            // v1
    _mongocrypt_buffer_t serverDerivedFromDataToken; // v2
} _FLE2EncryptedPayloadCommon_t;

static void _FLE2EncryptedPayloadCommon_cleanup(_FLE2EncryptedPayloadCommon_t *common) {
    if (!common) {
        return;
    }

    _mongocrypt_buffer_cleanup(&common->tokenKey);
    mc_CollectionsLevel1Token_destroy(common->collectionsLevel1Token);
    mc_ServerDataEncryptionLevel1Token_destroy(common->serverDataEncryptionLevel1Token);
    mc_ServerTokenDerivationLevel1Token_destroy(common->serverTokenDerivationLevel1Token);
    _mongocrypt_buffer_cleanup(&common->edcDerivedToken);
    _mongocrypt_buffer_cleanup(&common->escDerivedToken);
    _mongocrypt_buffer_cleanup(&common->eccDerivedToken);
    _mongocrypt_buffer_cleanup(&common->serverDerivedFromDataToken);
    memset(common, 0, sizeof(*common));
}

// _get_tokenKey returns the tokenKey identified by indexKeyId.
// Returns false on error.
static bool _get_tokenKey(_mongocrypt_key_broker_t *kb,
                          const _mongocrypt_buffer_t *indexKeyId,
                          _mongocrypt_buffer_t *tokenKey,
                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(indexKeyId);
    BSON_ASSERT_PARAM(tokenKey);

    _mongocrypt_buffer_t indexKey = {0};
    _mongocrypt_buffer_init(tokenKey);

    if (!_mongocrypt_key_broker_decrypted_key_by_id(kb, indexKeyId, &indexKey)) {
        CLIENT_ERR("unable to retrieve key");
        return false;
    }

    if (indexKey.len != MONGOCRYPT_KEY_LEN) {
        CLIENT_ERR("invalid indexKey, expected len=%" PRIu32 ", got len=%" PRIu32, MONGOCRYPT_KEY_LEN, indexKey.len);
        _mongocrypt_buffer_cleanup(&indexKey);
        return false;
    }

    // indexKey is 3 equal sized keys: [Ke][Km][TokenKey]
    BSON_ASSERT(MONGOCRYPT_KEY_LEN == (3 * MONGOCRYPT_TOKEN_KEY_LEN));
    if (!_mongocrypt_buffer_copy_from_data_and_size(tokenKey,
                                                    indexKey.data + (2 * MONGOCRYPT_TOKEN_KEY_LEN),
                                                    MONGOCRYPT_TOKEN_KEY_LEN)) {
        CLIENT_ERR("failed allocating memory for token key");
        _mongocrypt_buffer_cleanup(&indexKey);
        return false;
    }
    _mongocrypt_buffer_cleanup(&indexKey);
    return true;
}

static bool _mongocrypt_fle2_placeholder_common(_mongocrypt_key_broker_t *kb,
                                                _FLE2EncryptedPayloadCommon_t *ret,
                                                const _mongocrypt_buffer_t *indexKeyId,
                                                const _mongocrypt_buffer_t *value,
                                                bool useCounter,
                                                int64_t maxContentionCounter,
                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(ret);
    BSON_ASSERT_PARAM(indexKeyId);
    BSON_ASSERT_PARAM(value);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    _mongocrypt_buffer_t indexKey = {0};
    *ret = (_FLE2EncryptedPayloadCommon_t){{0}};

    if (!_get_tokenKey(kb, indexKeyId, &ret->tokenKey, status)) {
        goto fail;
    }

    ret->collectionsLevel1Token = mc_CollectionsLevel1Token_new(crypto, &ret->tokenKey, status);
    if (!ret->collectionsLevel1Token) {
        CLIENT_ERR("unable to derive collectionLevel1Token");
        goto fail;
    }

    ret->serverDataEncryptionLevel1Token = mc_ServerDataEncryptionLevel1Token_new(crypto, &ret->tokenKey, status);
    if (!ret->serverDataEncryptionLevel1Token) {
        CLIENT_ERR("unable to derive serverDataEncryptionLevel1Token");
        goto fail;
    }

    if (!_fle2_derive_EDC_token(crypto,
                                &ret->edcDerivedToken,
                                ret->collectionsLevel1Token,
                                value,
                                useCounter,
                                maxContentionCounter,
                                status)) {
        goto fail;
    }

    if (!_fle2_derive_ESC_token(crypto,
                                &ret->escDerivedToken,
                                ret->collectionsLevel1Token,
                                value,
                                useCounter,
                                maxContentionCounter,
                                status)) {
        goto fail;
    }

    if (kb->crypt->opts.use_fle2_v2) {
        /* FLE2v2 */
        ret->serverTokenDerivationLevel1Token = mc_ServerTokenDerivationLevel1Token_new(crypto, &ret->tokenKey, status);
        if (!ret->serverTokenDerivationLevel1Token) {
            CLIENT_ERR("unable to derive serverTokenDerivationLevel1Token");
            goto fail;
        }

        if (!_fle2_derive_serverDerivedFromDataToken(crypto,
                                                     &ret->serverDerivedFromDataToken,
                                                     ret->serverTokenDerivationLevel1Token,
                                                     value,
                                                     status)) {
            goto fail;
        }
    } else {
        /* FLE2v1 */
        if (!_fle2_derive_ECC_token(crypto,
                                    &ret->eccDerivedToken,
                                    ret->collectionsLevel1Token,
                                    value,
                                    useCounter,
                                    maxContentionCounter,
                                    status)) {
            goto fail;
        }
    }

    _mongocrypt_buffer_cleanup(&indexKey);
    return true;

fail:
    _FLE2EncryptedPayloadCommon_cleanup(ret);
    _mongocrypt_buffer_cleanup(&indexKey);
    return false;
}

// Shared implementation for insert/update and insert/update ForRange (v1)
static bool _mongocrypt_fle2_placeholder_to_insert_update_common_v1(_mongocrypt_key_broker_t *kb,
                                                                    mc_FLE2InsertUpdatePayload_t *out,
                                                                    int64_t *contentionFactor,
                                                                    _FLE2EncryptedPayloadCommon_t *common,
                                                                    const mc_FLE2EncryptionPlaceholder_t *placeholder,
                                                                    bson_iter_t *value_iter,
                                                                    mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(common);
    BSON_ASSERT_PARAM(placeholder);
    BSON_ASSERT_PARAM(value_iter);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(kb->crypt->opts.use_fle2_v2 == false);
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    _mongocrypt_buffer_t value = {0};
    bool res = false;

    *contentionFactor = 0;
    if (placeholder->maxContentionCounter > 0) {
        /* Choose a random contentionFactor in the inclusive range [0,
         * placeholder->maxContentionCounter] */
        if (!_mongocrypt_random_int64(crypto, placeholder->maxContentionCounter + 1, contentionFactor, status)) {
            goto fail;
        }
    }

    _mongocrypt_buffer_from_iter(&value, value_iter);
    if (!_mongocrypt_fle2_placeholder_common(kb,
                                             common,
                                             &placeholder->index_key_id,
                                             &value,
                                             true, /* derive tokens using counter */
                                             *contentionFactor,
                                             status)) {
        goto fail;
    }

    // d := EDCDerivedToken
    _mongocrypt_buffer_steal(&out->edcDerivedToken, &common->edcDerivedToken);
    // s := ESCDerivedToken
    _mongocrypt_buffer_steal(&out->escDerivedToken, &common->escDerivedToken);
    // c := ECCDerivedToken
    _mongocrypt_buffer_steal(&out->eccDerivedToken, &common->eccDerivedToken);

    // p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndCounter ||
    // ECCDerivedFromDataTokenAndCounter)
    if (!_fle2_derive_encrypted_token(crypto,
                                      &out->encryptedTokens,
                                      common->collectionsLevel1Token,
                                      &out->escDerivedToken,
                                      &out->eccDerivedToken,
                                      status)) {
        goto fail;
    }

    _mongocrypt_buffer_copy_to(&placeholder->index_key_id,
                               &out->indexKeyId); // u
    out->valueType = bson_iter_type(value_iter);  // t

    // v := UserKeyId + EncryptCTRAEAD(UserKey, value)
    {
        _mongocrypt_buffer_t ciphertext = {0};
        if (!_fle2_placeholder_aes_aead_encrypt(kb,
                                                _mcFLE2AEADAlgorithm(),
                                                &ciphertext,
                                                &placeholder->user_key_id,
                                                &value,
                                                status)) {
            goto fail;
        }
        const _mongocrypt_buffer_t v[2] = {placeholder->user_key_id, ciphertext};
        const bool ok = _mongocrypt_buffer_concat(&out->value, v, 2);
        _mongocrypt_buffer_cleanup(&ciphertext);
        if (!ok) {
            goto fail;
        }
    }

    // e := ServerDataEncryptionLevel1Token
    _mongocrypt_buffer_copy_to(mc_ServerDataEncryptionLevel1Token_get(common->serverDataEncryptionLevel1Token),
                               &out->serverEncryptionToken);

    res = true;
fail:
    _mongocrypt_buffer_cleanup(&value);
    return res;
}

/**
 * Payload subtype 4: FLE2InsertUpdatePayload
 *
 * {d: EDC, s: ESC, c: ECC,
 *  p: encToken, u: indexKeyId, t: type,
 *  v: value, e: serverToken}
 */
static bool _mongocrypt_fle2_placeholder_to_insert_update_ciphertext_v1(_mongocrypt_key_broker_t *kb,
                                                                        _mongocrypt_marking_t *marking,
                                                                        _mongocrypt_ciphertext_t *ciphertext,
                                                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT_PARAM(status);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(kb->crypt->opts.use_fle2_v2 == false);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
    BSON_ASSERT(marking->fle2.algorithm == MONGOCRYPT_FLE2_ALGORITHM_EQUALITY);

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    _FLE2EncryptedPayloadCommon_t common = {{0}};
    mc_FLE2InsertUpdatePayload_t payload;
    mc_FLE2InsertUpdatePayload_init(&payload);
    bool res = false;

    int64_t contentionFactor = 0; /* ignored */
    if (!_mongocrypt_fle2_placeholder_to_insert_update_common_v1(kb,
                                                                 &payload,
                                                                 &contentionFactor,
                                                                 &common,
                                                                 placeholder,
                                                                 &placeholder->v_iter,
                                                                 status)) {
        goto fail;
    }

    {
        bson_t out;
        bson_init(&out);
        mc_FLE2InsertUpdatePayload_serialize(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2InsertUpdatePayload.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2InsertUpdatePayload;

    res = true;
fail:
    mc_FLE2InsertUpdatePayload_cleanup(&payload);
    _FLE2EncryptedPayloadCommon_cleanup(&common);

    return res;
}

// Shared implementation for insert/update and insert/update ForRange (v2)
static bool _mongocrypt_fle2_placeholder_to_insert_update_common(_mongocrypt_key_broker_t *kb,
                                                                 mc_FLE2InsertUpdatePayloadV2_t *out,
                                                                 _FLE2EncryptedPayloadCommon_t *common,
                                                                 const mc_FLE2EncryptionPlaceholder_t *placeholder,
                                                                 bson_iter_t *value_iter,
                                                                 mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(common);
    BSON_ASSERT_PARAM(placeholder);
    BSON_ASSERT_PARAM(value_iter);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(kb->crypt->opts.use_fle2_v2 == true);
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    _mongocrypt_buffer_t value = {0};
    bool res = false;

    out->contentionFactor = 0; // k
    if (placeholder->maxContentionCounter > 0) {
        /* Choose a random contentionFactor in the inclusive range [0,
         * placeholder->maxContentionCounter] */
        if (!_mongocrypt_random_int64(crypto, placeholder->maxContentionCounter + 1, &out->contentionFactor, status)) {
            goto fail;
        }
    }

    _mongocrypt_buffer_from_iter(&value, value_iter);
    if (!_mongocrypt_fle2_placeholder_common(kb,
                                             common,
                                             &placeholder->index_key_id,
                                             &value,
                                             true, /* derive tokens using counter */
                                             out->contentionFactor,
                                             status)) {
        goto fail;
    }

    // d := EDCDerivedToken
    _mongocrypt_buffer_steal(&out->edcDerivedToken, &common->edcDerivedToken);
    // s := ESCDerivedToken
    _mongocrypt_buffer_steal(&out->escDerivedToken, &common->escDerivedToken);
    BSON_ASSERT(common->eccDerivedToken.data == NULL);

    // p := EncryptCBC(ECOCToken, ESCDerivedFromDataTokenAndCounter)
    if (!_fle2_derive_encrypted_token(crypto,
                                      &out->encryptedTokens,
                                      common->collectionsLevel1Token,
                                      &out->escDerivedToken,
                                      NULL, // unused in v2
                                      status)) {
        goto fail;
    }

    _mongocrypt_buffer_copy_to(&placeholder->index_key_id,
                               &out->indexKeyId); // u
    out->valueType = bson_iter_type(value_iter);  // t

    // v := UserKeyId + EncryptCBCAEAD(UserKey, value)
    {
        _mongocrypt_buffer_t ciphertext = {0};
        if (!_fle2_placeholder_aes_aead_encrypt(kb,
                                                _mcFLE2v2AEADAlgorithm(),
                                                &ciphertext,
                                                &placeholder->user_key_id,
                                                &value,
                                                status)) {
            goto fail;
        }
        const _mongocrypt_buffer_t v[2] = {placeholder->user_key_id, ciphertext};
        const bool ok = _mongocrypt_buffer_concat(&out->value, v, 2);
        _mongocrypt_buffer_cleanup(&ciphertext);
        if (!ok) {
            goto fail;
        }
    }

    // e := ServerDataEncryptionLevel1Token
    _mongocrypt_buffer_copy_to(mc_ServerDataEncryptionLevel1Token_get(common->serverDataEncryptionLevel1Token),
                               &out->serverEncryptionToken);

    // l := ServerDerivedFromDataToken
    _mongocrypt_buffer_steal(&out->serverDerivedFromDataToken, &common->serverDerivedFromDataToken);

    res = true;
fail:
    _mongocrypt_buffer_cleanup(&value);
    return res;
}

/**
 * Payload subtype 11: FLE2InsertUpdatePayloadV2
 * Delegates to ..._insert_update_ciphertext_v1 for subtype 4
 *   when crypt.opts.use_fle2_v2 == false
 *
 * {d: EDC, s: ESC, p: encToken,
 *  u: indexKeyId, t: valueType, v: value,
 *  e: serverToken, l: serverDerivedFromDataToken,
 *  k: contentionFactor}
 */
static bool _mongocrypt_fle2_placeholder_to_insert_update_ciphertext(_mongocrypt_key_broker_t *kb,
                                                                     _mongocrypt_marking_t *marking,
                                                                     _mongocrypt_ciphertext_t *ciphertext,
                                                                     mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);

    if (!kb->crypt->opts.use_fle2_v2) {
        return _mongocrypt_fle2_placeholder_to_insert_update_ciphertext_v1(kb, marking, ciphertext, status);
    }

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    _FLE2EncryptedPayloadCommon_t common = {{0}};
    mc_FLE2InsertUpdatePayloadV2_t payload;
    mc_FLE2InsertUpdatePayloadV2_init(&payload);
    bool res = false;

    if (!_mongocrypt_fle2_placeholder_to_insert_update_common(kb,
                                                              &payload,
                                                              &common,
                                                              placeholder,
                                                              &placeholder->v_iter,
                                                              status)) {
        goto fail;
    }

    {
        bson_t out;
        bson_init(&out);
        mc_FLE2InsertUpdatePayloadV2_serialize(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }

    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2InsertUpdatePayloadV2.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2InsertUpdatePayloadV2;

    res = true;
fail:
    mc_FLE2InsertUpdatePayloadV2_cleanup(&payload);
    _FLE2EncryptedPayloadCommon_cleanup(&common);

    return res;
}

// get_edges creates and returns edges from an FLE2RangeInsertSpec. Returns NULL
// on error.
static mc_edges_t *get_edges(mc_FLE2RangeInsertSpec_t *insertSpec, size_t sparsity, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(insertSpec);

    bson_type_t value_type = bson_iter_type(&insertSpec->v);

    if (value_type == BSON_TYPE_INT32) {
        return mc_getEdgesInt32((mc_getEdgesInt32_args_t){.value = bson_iter_int32(&insertSpec->v),
                                                          .min = OPT_I32(bson_iter_int32(&insertSpec->min)),
                                                          .max = OPT_I32(bson_iter_int32(&insertSpec->max)),
                                                          .sparsity = sparsity},
                                status);
    }

    else if (value_type == BSON_TYPE_INT64) {
        return mc_getEdgesInt64((mc_getEdgesInt64_args_t){.value = bson_iter_int64(&insertSpec->v),
                                                          .min = OPT_I64(bson_iter_int64(&insertSpec->min)),
                                                          .max = OPT_I64(bson_iter_int64(&insertSpec->max)),
                                                          .sparsity = sparsity},
                                status);
    }

    else if (value_type == BSON_TYPE_DATE_TIME) {
        return mc_getEdgesInt64((mc_getEdgesInt64_args_t){.value = bson_iter_date_time(&insertSpec->v),
                                                          .min = OPT_I64(bson_iter_date_time(&insertSpec->min)),
                                                          .max = OPT_I64(bson_iter_date_time(&insertSpec->max)),
                                                          .sparsity = sparsity},
                                status);
    }

    else if (value_type == BSON_TYPE_DOUBLE) {
        mc_getEdgesDouble_args_t args = {.value = bson_iter_double(&insertSpec->v), .sparsity = sparsity};
        if (insertSpec->precision.set) {
            // If precision is set, pass min/max/precision to mc_getEdgesDouble.
            // Do not pass min/max if precision is not set. All three must be set
            // or all three must be unset in mc_getTypeInfoDouble.
            args.min = OPT_DOUBLE(bson_iter_double(&insertSpec->min));
            args.max = OPT_DOUBLE(bson_iter_double(&insertSpec->max));
            args.precision = insertSpec->precision;
        }

        return mc_getEdgesDouble(args, status);
    }

    else if (value_type == BSON_TYPE_DECIMAL128) {
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
        const mc_dec128 value = mc_dec128_from_bson_iter(&insertSpec->v);
        mc_getEdgesDecimal128_args_t args = {
            .value = value,
            .sparsity = sparsity,
        };
        if (insertSpec->precision.set) {
            const mc_dec128 min = mc_dec128_from_bson_iter(&insertSpec->min);
            const mc_dec128 max = mc_dec128_from_bson_iter(&insertSpec->max);
            args.min = OPT_MC_DEC128(min);
            args.max = OPT_MC_DEC128(max);
            args.precision = insertSpec->precision;
        }
        return mc_getEdgesDecimal128(args, status);
#else // ↑↑↑↑↑↑↑↑ With Decimal128 / Without ↓↓↓↓↓↓↓↓↓↓
        CLIENT_ERR("unsupported BSON type (Decimal128) for range: libmongocrypt "
                   "was built without extended Decimal128 support");
        return NULL;
#endif
    }

    CLIENT_ERR("unsupported BSON type: %s for range", mc_bson_type_to_string(value_type));
    return NULL;
}

/**
 * Payload subtype 4: FLE2InsertUpdatePayload for range updates
 *
 * {d: EDC, s: ESC, c: ECC,
 *  p: encToken, u: indexKeyId, t: type,
 *  v: value, e: serverToken,
 *  g: [{d: EDC, s: ESC, c: ECC, p: encToken},
 *      {d: EDC, s: ESC, c: ECC, p: encToken},
 *      ...]}
 */
static bool _mongocrypt_fle2_placeholder_to_insert_update_ciphertextForRange_v1(_mongocrypt_key_broker_t *kb,
                                                                                _mongocrypt_marking_t *marking,
                                                                                _mongocrypt_ciphertext_t *ciphertext,
                                                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT_PARAM(status);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(kb->crypt->opts.use_fle2_v2 == false);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
    BSON_ASSERT(marking->fle2.algorithm == MONGOCRYPT_FLE2_ALGORITHM_RANGE);

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    _FLE2EncryptedPayloadCommon_t common = {{0}};
    mc_FLE2InsertUpdatePayload_t payload;
    mc_FLE2InsertUpdatePayload_init(&payload);
    bool res = false;
    mc_edges_t *edges = NULL;

    // Parse the value ("v"), min ("min"), and max ("max") from
    // FLE2EncryptionPlaceholder for range insert.
    mc_FLE2RangeInsertSpec_t insertSpec;
    if (!mc_FLE2RangeInsertSpec_parse(&insertSpec, &placeholder->v_iter, status)) {
        goto fail;
    }

    int64_t contentionFactor = 0;
    if (!_mongocrypt_fle2_placeholder_to_insert_update_common_v1(kb,
                                                                 &payload,
                                                                 &contentionFactor,
                                                                 &common,
                                                                 &marking->fle2,
                                                                 &insertSpec.v,
                                                                 status)) {
        goto fail;
    }

    // g:= array<EdgeTokenSet>
    {
        BSON_ASSERT(placeholder->sparsity >= 0 && (uint64_t)placeholder->sparsity <= (uint64_t)SIZE_MAX);
        edges = get_edges(&insertSpec, (size_t)placeholder->sparsity, status);
        if (!edges) {
            goto fail;
        }

        for (size_t i = 0; i < mc_edges_len(edges); ++i) {
            // Create an EdgeTokenSet from each edge.
            bool loop_ok = false;
            const char *edge = mc_edges_get(edges, i);
            _mongocrypt_buffer_t edge_buf = {0};
            _FLE2EncryptedPayloadCommon_t edge_tokens = {{0}};
            _mongocrypt_buffer_t encryptedTokens = {0};
            mc_EdgeTokenSet_t etc = {{0}};

            if (!_mongocrypt_buffer_from_string(&edge_buf, edge)) {
                CLIENT_ERR("failed to copy edge to buffer");
                goto fail_loop;
            }

            if (!_mongocrypt_fle2_placeholder_common(kb,
                                                     &edge_tokens,
                                                     &placeholder->index_key_id,
                                                     &edge_buf,
                                                     true, /* derive tokens using counter */
                                                     contentionFactor,
                                                     status)) {
                goto fail_loop;
            }

            // d := EDCDerivedToken
            _mongocrypt_buffer_steal(&etc.edcDerivedToken, &edge_tokens.edcDerivedToken);
            // s := ESCDerivedToken
            _mongocrypt_buffer_steal(&etc.escDerivedToken, &edge_tokens.escDerivedToken);
            // c := ECCDerivedToken
            _mongocrypt_buffer_steal(&etc.eccDerivedToken, &edge_tokens.eccDerivedToken);

            // p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndCounter ||
            // ECCDerivedFromDataTokenAndCounter)
            if (!_fle2_derive_encrypted_token(kb->crypt->crypto,
                                              &etc.encryptedTokens,
                                              edge_tokens.collectionsLevel1Token,
                                              &etc.escDerivedToken,
                                              &etc.eccDerivedToken,
                                              status)) {
                goto fail_loop;
            }

            _mc_array_append_val(&payload.edgeTokenSetArray, etc);

            loop_ok = true;
        fail_loop:
            _mongocrypt_buffer_cleanup(&encryptedTokens);
            _FLE2EncryptedPayloadCommon_cleanup(&edge_tokens);
            _mongocrypt_buffer_cleanup(&edge_buf);
            if (!loop_ok) {
                goto fail;
            }
        }
    }

    {
        bson_t out;
        bson_init(&out);
        mc_FLE2InsertUpdatePayload_serializeForRange(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2InsertUpdatePayload.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2InsertUpdatePayload;

    res = true;
fail:
    mc_edges_destroy(edges);
    mc_FLE2InsertUpdatePayload_cleanup(&payload);
    _FLE2EncryptedPayloadCommon_cleanup(&common);

    return res;
}

/**
 * Payload subtype 11: FLE2InsertUpdatePayloadV2 for range updates
 * Delegates to ..._insert_update_ciphertextForRange_v1 for subtype 4
 *   when crypt.opts.use_fle2_v2 == false
 *
 * {d: EDC, s: ESC, p: encToken,
 *  u: indexKeyId, t: valueType, v: value,
 *  e: serverToken, l: serverDerivedFromDataToken,
 *  k: contentionFactor,
 *  g: [{d: EDC, s: ESC, l: serverDerivedFromDataToken, p: encToken},
 *      {d: EDC, s: ESC, l: serverDerivedFromDataToken, p: encToken},
 *      ...]}
 */
static bool _mongocrypt_fle2_placeholder_to_insert_update_ciphertextForRange(_mongocrypt_key_broker_t *kb,
                                                                             _mongocrypt_marking_t *marking,
                                                                             _mongocrypt_ciphertext_t *ciphertext,
                                                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);

    if (!kb->crypt->opts.use_fle2_v2) {
        return _mongocrypt_fle2_placeholder_to_insert_update_ciphertextForRange_v1(kb, marking, ciphertext, status);
    }

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    _FLE2EncryptedPayloadCommon_t common = {{0}};
    mc_FLE2InsertUpdatePayloadV2_t payload;
    mc_FLE2InsertUpdatePayloadV2_init(&payload);
    bool res = false;
    mc_edges_t *edges = NULL;

    // Parse the value ("v"), min ("min"), and max ("max") from
    // FLE2EncryptionPlaceholder for range insert.
    mc_FLE2RangeInsertSpec_t insertSpec;
    if (!mc_FLE2RangeInsertSpec_parse(&insertSpec, &placeholder->v_iter, status)) {
        goto fail;
    }

    if (!_mongocrypt_fle2_placeholder_to_insert_update_common(kb,
                                                              &payload,
                                                              &common,
                                                              &marking->fle2,
                                                              &insertSpec.v,
                                                              status)) {
        goto fail;
    }

    // g:= array<EdgeTokenSetV2>
    {
        BSON_ASSERT(placeholder->sparsity >= 0 && (uint64_t)placeholder->sparsity <= (uint64_t)SIZE_MAX);
        edges = get_edges(&insertSpec, (size_t)placeholder->sparsity, status);
        if (!edges) {
            goto fail;
        }

        for (size_t i = 0; i < mc_edges_len(edges); ++i) {
            // Create an EdgeTokenSet from each edge.
            bool loop_ok = false;
            const char *edge = mc_edges_get(edges, i);
            _mongocrypt_buffer_t edge_buf = {0};
            _FLE2EncryptedPayloadCommon_t edge_tokens = {{0}};
            _mongocrypt_buffer_t encryptedTokens = {0};
            mc_EdgeTokenSetV2_t etc = {{0}};

            if (!_mongocrypt_buffer_from_string(&edge_buf, edge)) {
                CLIENT_ERR("failed to copy edge to buffer");
                goto fail_loop;
            }

            if (!_mongocrypt_fle2_placeholder_common(kb,
                                                     &edge_tokens,
                                                     &placeholder->index_key_id,
                                                     &edge_buf,
                                                     true, /* derive tokens using counter */
                                                     payload.contentionFactor,
                                                     status)) {
                goto fail_loop;
            }
            BSON_ASSERT(edge_tokens.eccDerivedToken.data == NULL);

            // d := EDCDerivedToken
            _mongocrypt_buffer_steal(&etc.edcDerivedToken, &edge_tokens.edcDerivedToken);
            // s := ESCDerivedToken
            _mongocrypt_buffer_steal(&etc.escDerivedToken, &edge_tokens.escDerivedToken);

            // l := serverDerivedFromDataToken
            _mongocrypt_buffer_steal(&etc.serverDerivedFromDataToken, &edge_tokens.serverDerivedFromDataToken);

            // p := EncryptCBC(ECOCToken, ESCDerivedFromDataTokenAndCounter)
            if (!_fle2_derive_encrypted_token(kb->crypt->crypto,
                                              &etc.encryptedTokens,
                                              edge_tokens.collectionsLevel1Token,
                                              &etc.escDerivedToken,
                                              NULL, // ecc unsed in FLE2v2
                                              status)) {
                goto fail_loop;
            }

            _mc_array_append_val(&payload.edgeTokenSetArray, etc);

            loop_ok = true;
        fail_loop:
            _mongocrypt_buffer_cleanup(&encryptedTokens);
            _FLE2EncryptedPayloadCommon_cleanup(&edge_tokens);
            _mongocrypt_buffer_cleanup(&edge_buf);
            if (!loop_ok) {
                goto fail;
            }
        }
    }

    {
        bson_t out;
        bson_init(&out);
        mc_FLE2InsertUpdatePayloadV2_serializeForRange(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2InsertUpdatePayloadV2.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2InsertUpdatePayloadV2;

    res = true;
fail:
    mc_edges_destroy(edges);
    mc_FLE2InsertUpdatePayloadV2_cleanup(&payload);
    _FLE2EncryptedPayloadCommon_cleanup(&common);

    return res;
}

/**
 * Payload subtype 5: FLE2FindEqualityPayload
 *
 * {d: EDC, s: ESC, c: ECC, e: serverToken, cm: contentionCounter}
 */
static bool _mongocrypt_fle2_placeholder_to_find_ciphertext_v1(_mongocrypt_key_broker_t *kb,
                                                               _mongocrypt_marking_t *marking,
                                                               _mongocrypt_ciphertext_t *ciphertext,
                                                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);

    _FLE2EncryptedPayloadCommon_t common = {{0}};
    _mongocrypt_buffer_t value = {0};
    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    mc_FLE2FindEqualityPayload_t payload;
    bool res = false;

    BSON_ASSERT(kb->crypt->opts.use_fle2_v2 == false);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND);
    _mongocrypt_buffer_init(&value);
    mc_FLE2FindEqualityPayload_init(&payload);

    _mongocrypt_buffer_from_iter(&value, &placeholder->v_iter);

    if (!_mongocrypt_fle2_placeholder_common(kb,
                                             &common,
                                             &placeholder->index_key_id,
                                             &value,
                                             false, /* derive tokens without counter */
                                             placeholder->maxContentionCounter,
                                             status)) {
        goto fail;
    }

    // d := EDCDerivedToken
    _mongocrypt_buffer_steal(&payload.edcDerivedToken, &common.edcDerivedToken);
    // s := ESCDerivedToken
    _mongocrypt_buffer_steal(&payload.escDerivedToken, &common.escDerivedToken);
    // c := ECCDerivedToken
    _mongocrypt_buffer_steal(&payload.eccDerivedToken, &common.eccDerivedToken);

    // e := ServerDataEncryptionLevel1Token
    _mongocrypt_buffer_copy_to(mc_ServerDataEncryptionLevel1Token_get(common.serverDataEncryptionLevel1Token),
                               &payload.serverEncryptionToken);

    payload.maxContentionCounter = placeholder->maxContentionCounter;

    {
        bson_t out;
        bson_init(&out);
        mc_FLE2FindEqualityPayload_serialize(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2FindEqualityPayload.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2FindEqualityPayload;

    res = true;
fail:
    mc_FLE2FindEqualityPayload_cleanup(&payload);
    _mongocrypt_buffer_cleanup(&value);
    _FLE2EncryptedPayloadCommon_cleanup(&common);

    return res;
}

/**
 * Payload subtype 12: FLE2FindEqualityPayloadV2
 * Delegates to ..._find_ciphertext_v1 when crypt->opts.use_fle2_v2 == false.
 *
 * {d: EDC, s: ESC, l: serverDerivedFromDataToken, cm: contentionCounter}
 */
static bool _mongocrypt_fle2_placeholder_to_find_ciphertext(_mongocrypt_key_broker_t *kb,
                                                            _mongocrypt_marking_t *marking,
                                                            _mongocrypt_ciphertext_t *ciphertext,
                                                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);

    if (kb->crypt->opts.use_fle2_v2 == false) {
        return _mongocrypt_fle2_placeholder_to_find_ciphertext_v1(kb, marking, ciphertext, status);
    }

    _FLE2EncryptedPayloadCommon_t common = {{0}};
    _mongocrypt_buffer_t value = {0};
    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    mc_FLE2FindEqualityPayloadV2_t payload;
    bool res = false;

    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND);

    _mongocrypt_buffer_init(&value);
    mc_FLE2FindEqualityPayloadV2_init(&payload);

    _mongocrypt_buffer_from_iter(&value, &placeholder->v_iter);

    if (!_mongocrypt_fle2_placeholder_common(kb,
                                             &common,
                                             &placeholder->index_key_id,
                                             &value,
                                             false, /* derive tokens without counter */
                                             placeholder->maxContentionCounter,
                                             status)) {
        goto fail;
    }
    BSON_ASSERT(common.eccDerivedToken.data == NULL);

    // d := EDCDerivedToken
    _mongocrypt_buffer_steal(&payload.edcDerivedToken, &common.edcDerivedToken);
    // s := ESCDerivedToken
    _mongocrypt_buffer_steal(&payload.escDerivedToken, &common.escDerivedToken);
    // l := serverDerivedFromDataToken
    _mongocrypt_buffer_steal(&payload.serverDerivedFromDataToken, &common.serverDerivedFromDataToken);

    // cm := maxContentionCounter
    payload.maxContentionCounter = placeholder->maxContentionCounter;

    {
        bson_t out;
        bson_init(&out);
        mc_FLE2FindEqualityPayloadV2_serialize(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2FindEqualityPayloadV2.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2FindEqualityPayloadV2;

    res = true;
fail:
    mc_FLE2FindEqualityPayloadV2_cleanup(&payload);
    _mongocrypt_buffer_cleanup(&value);
    _FLE2EncryptedPayloadCommon_cleanup(&common);

    return res;
}

static bool isInfinite(bson_iter_t *iter) {
    return mc_isinf(bson_iter_double(iter));
}

// mc_get_mincover_from_FLE2RangeFindSpec creates and returns a mincover from an
// FLE2RangeFindSpec. Returns NULL on error.
mc_mincover_t *
mc_get_mincover_from_FLE2RangeFindSpec(mc_FLE2RangeFindSpec_t *findSpec, size_t sparsity, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(findSpec);
    BSON_ASSERT(findSpec->edgesInfo.set);

    bson_type_t bsonType = bson_iter_type(&findSpec->edgesInfo.value.indexMin);

    if (bson_iter_type(&findSpec->edgesInfo.value.indexMin) != bson_iter_type(&findSpec->edgesInfo.value.indexMax)) {
        CLIENT_ERR("indexMin and indexMax must have the same type. Got: %s indexMin and "
                   "%s indexMax",
                   mc_bson_type_to_string(bson_iter_type(&findSpec->edgesInfo.value.indexMin)),
                   mc_bson_type_to_string(bson_iter_type(&findSpec->edgesInfo.value.indexMax)));
        return NULL;
    }

    bson_iter_t lowerBound = findSpec->edgesInfo.value.lowerBound;
    bson_iter_t upperBound = findSpec->edgesInfo.value.upperBound;
    bool includeLowerBound = findSpec->edgesInfo.value.lbIncluded;
    bool includeUpperBound = findSpec->edgesInfo.value.ubIncluded;

    // Open-ended ranges are represented with infinity as the other endpoint.
    // Resolve infinite bounds at this point to end at the min or max for this
    // index.
    if (isInfinite(&lowerBound)) {
        lowerBound = findSpec->edgesInfo.value.indexMin;
        includeLowerBound = true;
    }
    if (isInfinite(&upperBound)) {
        upperBound = findSpec->edgesInfo.value.indexMax;
        includeUpperBound = true;
    }

    if (bson_iter_type(&lowerBound) != bsonType) {
        CLIENT_ERR("expected lowerBound to match index type %s, got %s",
                   mc_bson_type_to_string(bsonType),
                   mc_bson_type_to_string(bson_iter_type(&lowerBound)));
        return NULL;
    }

    if (bson_iter_type(&upperBound) != bsonType) {
        CLIENT_ERR("expected upperBound to match index type %s, got %s",
                   mc_bson_type_to_string(bsonType),
                   mc_bson_type_to_string(bson_iter_type(&upperBound)));
        return NULL;
    }

    switch (bsonType) {
    case BSON_TYPE_INT32:
        BSON_ASSERT(bson_iter_type(&lowerBound) == BSON_TYPE_INT32);
        BSON_ASSERT(bson_iter_type(&upperBound) == BSON_TYPE_INT32);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMin) == BSON_TYPE_INT32);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMax) == BSON_TYPE_INT32);
        return mc_getMincoverInt32(
            (mc_getMincoverInt32_args_t){.lowerBound = bson_iter_int32(&lowerBound),
                                         .includeLowerBound = includeLowerBound,
                                         .upperBound = bson_iter_int32(&upperBound),
                                         .includeUpperBound = includeUpperBound,
                                         .min = OPT_I32(bson_iter_int32(&findSpec->edgesInfo.value.indexMin)),
                                         .max = OPT_I32(bson_iter_int32(&findSpec->edgesInfo.value.indexMax)),
                                         .sparsity = sparsity},
            status);

    case BSON_TYPE_INT64:
        BSON_ASSERT(bson_iter_type(&lowerBound) == BSON_TYPE_INT64);
        BSON_ASSERT(bson_iter_type(&upperBound) == BSON_TYPE_INT64);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMin) == BSON_TYPE_INT64);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMax) == BSON_TYPE_INT64);
        return mc_getMincoverInt64(
            (mc_getMincoverInt64_args_t){.lowerBound = bson_iter_int64(&lowerBound),
                                         .includeLowerBound = includeLowerBound,
                                         .upperBound = bson_iter_int64(&upperBound),
                                         .includeUpperBound = includeUpperBound,
                                         .min = OPT_I64(bson_iter_int64(&findSpec->edgesInfo.value.indexMin)),
                                         .max = OPT_I64(bson_iter_int64(&findSpec->edgesInfo.value.indexMax)),
                                         .sparsity = sparsity},
            status);
    case BSON_TYPE_DATE_TIME:
        BSON_ASSERT(bson_iter_type(&lowerBound) == BSON_TYPE_DATE_TIME);
        BSON_ASSERT(bson_iter_type(&upperBound) == BSON_TYPE_DATE_TIME);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMin) == BSON_TYPE_DATE_TIME);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMax) == BSON_TYPE_DATE_TIME);
        return mc_getMincoverInt64(
            (mc_getMincoverInt64_args_t){.lowerBound = bson_iter_date_time(&lowerBound),
                                         .includeLowerBound = includeLowerBound,
                                         .upperBound = bson_iter_date_time(&upperBound),
                                         .includeUpperBound = includeUpperBound,
                                         .min = OPT_I64(bson_iter_date_time(&findSpec->edgesInfo.value.indexMin)),
                                         .max = OPT_I64(bson_iter_date_time(&findSpec->edgesInfo.value.indexMax)),
                                         .sparsity = sparsity},
            status);
    case BSON_TYPE_DOUBLE: {
        BSON_ASSERT(bson_iter_type(&lowerBound) == BSON_TYPE_DOUBLE);
        BSON_ASSERT(bson_iter_type(&upperBound) == BSON_TYPE_DOUBLE);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMin) == BSON_TYPE_DOUBLE);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMax) == BSON_TYPE_DOUBLE);

        mc_getMincoverDouble_args_t args = {.lowerBound = bson_iter_double(&lowerBound),
                                            .includeLowerBound = includeLowerBound,
                                            .upperBound = bson_iter_double(&upperBound),
                                            .includeUpperBound = includeUpperBound,
                                            .sparsity = sparsity};
        if (findSpec->edgesInfo.value.precision.set) {
            // If precision is set, pass min/max/precision to mc_getMincoverDouble.
            // Do not pass min/max if precision is not set. All three must be set
            // or all three must be unset in mc_getTypeInfoDouble.
            args.min = OPT_DOUBLE(bson_iter_double(&findSpec->edgesInfo.value.indexMin));
            args.max = OPT_DOUBLE(bson_iter_double(&findSpec->edgesInfo.value.indexMax));
            args.precision = findSpec->edgesInfo.value.precision;
        }
        return mc_getMincoverDouble(args, status);
    }
    case BSON_TYPE_DECIMAL128: {
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT
        BSON_ASSERT(bson_iter_type(&lowerBound) == BSON_TYPE_DECIMAL128);
        BSON_ASSERT(bson_iter_type(&upperBound) == BSON_TYPE_DECIMAL128);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMin) == BSON_TYPE_DECIMAL128);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMax) == BSON_TYPE_DECIMAL128);

        mc_getMincoverDecimal128_args_t args = {
            .lowerBound = mc_dec128_from_bson_iter(&lowerBound),
            .includeLowerBound = includeLowerBound,
            .upperBound = mc_dec128_from_bson_iter(&upperBound),
            .includeUpperBound = includeUpperBound,
            .sparsity = sparsity,
        };
        if (findSpec->edgesInfo.value.precision.set) {
            args.min = OPT_MC_DEC128(mc_dec128_from_bson_iter(&findSpec->edgesInfo.value.indexMin));
            args.max = OPT_MC_DEC128(mc_dec128_from_bson_iter(&findSpec->edgesInfo.value.indexMax));
            args.precision = findSpec->edgesInfo.value.precision;
        }
        return mc_getMincoverDecimal128(args, status);
#else // ↑↑↑↑↑↑↑↑ With Decimal128 / Without ↓↓↓↓↓↓↓↓↓↓
        CLIENT_ERR("FLE2 find is not supported for Decimal128: libmongocrypt "
                   "was built without Decimal128 support");
        return NULL;
#endif
    }

    case BSON_TYPE_EOD:
    case BSON_TYPE_UTF8:
    case BSON_TYPE_DOCUMENT:
    case BSON_TYPE_ARRAY:
    case BSON_TYPE_BINARY:
    case BSON_TYPE_UNDEFINED:
    case BSON_TYPE_OID:
    case BSON_TYPE_BOOL:
    case BSON_TYPE_NULL:
    case BSON_TYPE_REGEX:
    case BSON_TYPE_DBPOINTER:
    case BSON_TYPE_CODE:
    case BSON_TYPE_SYMBOL:
    case BSON_TYPE_CODEWSCOPE:
    case BSON_TYPE_TIMESTAMP:
    case BSON_TYPE_MAXKEY:
    case BSON_TYPE_MINKEY:
    default: CLIENT_ERR("FLE2 find is not supported for type: %s", mc_bson_type_to_string(bsonType)); return NULL;
    }
}

/**
 * Payload subtype 10: FLE2FindRangePayload
 *
 * {e: serverToken, cm: contentionCounter,
 *  g: [{d: EDC, s: ESC, c: ECC}, ...]}
 */
static bool _mongocrypt_fle2_placeholder_to_find_ciphertextForRange_v1(_mongocrypt_key_broker_t *kb,
                                                                       _mongocrypt_marking_t *marking,
                                                                       _mongocrypt_ciphertext_t *ciphertext,
                                                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT(kb->crypt);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    mc_FLE2FindRangePayload_t payload;
    bool res = false;
    mc_mincover_t *mincover = NULL;
    _mongocrypt_buffer_t tokenKey = {0};

    BSON_ASSERT(kb->crypt->opts.use_fle2_v2 == false);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
    BSON_ASSERT(placeholder);
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND);
    BSON_ASSERT(placeholder->algorithm == MONGOCRYPT_FLE2_ALGORITHM_RANGE);
    mc_FLE2FindRangePayload_init(&payload);

    // Parse the query bounds and index bounds from FLE2EncryptionPlaceholder for
    // range find.
    mc_FLE2RangeFindSpec_t findSpec;
    if (!mc_FLE2RangeFindSpec_parse(&findSpec, &placeholder->v_iter, status)) {
        goto fail;
    }

    if (findSpec.edgesInfo.set) {
        // cm := Queryable Encryption max counter
        payload.payload.value.maxContentionCounter = placeholder->maxContentionCounter;

        // e := ServerDataEncryptionLevel1Token
        {
            if (!_get_tokenKey(kb, &placeholder->index_key_id, &tokenKey, status)) {
                goto fail;
            }

            mc_ServerDataEncryptionLevel1Token_t *serverToken =
                mc_ServerDataEncryptionLevel1Token_new(crypto, &tokenKey, status);
            if (!serverToken) {
                goto fail;
            }
            _mongocrypt_buffer_copy_to(mc_ServerDataEncryptionLevel1Token_get(serverToken),
                                       &payload.payload.value.serverEncryptionToken);
            mc_ServerDataEncryptionLevel1Token_destroy(serverToken);
        }

        // g:= array<EdgeFindTokenSet>
        {
            BSON_ASSERT(placeholder->sparsity >= 0 && (uint64_t)placeholder->sparsity <= (uint64_t)SIZE_MAX);
            mincover = mc_get_mincover_from_FLE2RangeFindSpec(&findSpec, (size_t)placeholder->sparsity, status);
            if (!mincover) {
                goto fail;
            }

            for (size_t i = 0; i < mc_mincover_len(mincover); i++) {
                // Create a EdgeFindTokenSet from each edge.
                bool loop_ok = false;
                const char *edge = mc_mincover_get(mincover, i);
                _mongocrypt_buffer_t edge_buf = {0};
                _FLE2EncryptedPayloadCommon_t edge_tokens = {{0}};
                mc_EdgeFindTokenSet_t eftc = {{0}};

                if (!_mongocrypt_buffer_from_string(&edge_buf, edge)) {
                    CLIENT_ERR("failed to copy edge to buffer");
                    goto fail_loop;
                }

                if (!_mongocrypt_fle2_placeholder_common(kb,
                                                         &edge_tokens,
                                                         &placeholder->index_key_id,
                                                         &edge_buf,
                                                         false, /* derive tokens using counter */
                                                         placeholder->maxContentionCounter,
                                                         status)) {
                    goto fail_loop;
                }

                // d := EDCDerivedToken
                _mongocrypt_buffer_steal(&eftc.edcDerivedToken, &edge_tokens.edcDerivedToken);
                // s := ESCDerivedToken
                _mongocrypt_buffer_steal(&eftc.escDerivedToken, &edge_tokens.escDerivedToken);
                // c := ECCDerivedToken
                _mongocrypt_buffer_steal(&eftc.eccDerivedToken, &edge_tokens.eccDerivedToken);

                _mc_array_append_val(&payload.payload.value.edgeFindTokenSetArray, eftc);

                loop_ok = true;
            fail_loop:
                _FLE2EncryptedPayloadCommon_cleanup(&edge_tokens);
                _mongocrypt_buffer_cleanup(&edge_buf);
                if (!loop_ok) {
                    goto fail;
                }
            }
        }
        payload.payload.set = true;
    }

    payload.payloadId = findSpec.payloadId;
    payload.firstOperator = findSpec.firstOperator;
    payload.secondOperator = findSpec.secondOperator;

    // Serialize.
    {
        bson_t out = BSON_INITIALIZER;
        mc_FLE2FindRangePayload_serialize(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    _mongocrypt_buffer_steal(&ciphertext->key_id, &placeholder->index_key_id);

    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2FindRangePayload.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2FindRangePayload;

    res = true;
fail:
    mc_mincover_destroy(mincover);
    mc_FLE2FindRangePayload_cleanup(&payload);
    _mongocrypt_buffer_cleanup(&tokenKey);

    return res;
}

/**
 * Payload subtype 13: FLE2FindRangePayloadV2
 * Delegates to ..._find_ciphertextForRange_v1
 *   when crypt->opts.use_fle2_v2 is false
 *
 * {cm: contentionCounter,
 *  g: [{d: EDC, s: ESC, l: serverDerivedFromDataToken}, ...]}
 */
static bool _mongocrypt_fle2_placeholder_to_find_ciphertextForRange(_mongocrypt_key_broker_t *kb,
                                                                    _mongocrypt_marking_t *marking,
                                                                    _mongocrypt_ciphertext_t *ciphertext,
                                                                    mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);

    if (kb->crypt->opts.use_fle2_v2 == false) {
        return _mongocrypt_fle2_placeholder_to_find_ciphertextForRange_v1(kb, marking, ciphertext, status);
    }

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    mc_FLE2FindRangePayloadV2_t payload;
    bool res = false;
    mc_mincover_t *mincover = NULL;
    _mongocrypt_buffer_t tokenKey = {0};

    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
    BSON_ASSERT(placeholder);
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND);
    BSON_ASSERT(placeholder->algorithm == MONGOCRYPT_FLE2_ALGORITHM_RANGE);
    mc_FLE2FindRangePayloadV2_init(&payload);

    // Parse the query bounds and index bounds from FLE2EncryptionPlaceholder for
    // range find.
    mc_FLE2RangeFindSpec_t findSpec;
    if (!mc_FLE2RangeFindSpec_parse(&findSpec, &placeholder->v_iter, status)) {
        goto fail;
    }

    if (findSpec.edgesInfo.set) {
        // cm := Queryable Encryption max counter
        payload.payload.value.maxContentionCounter = placeholder->maxContentionCounter;

        // g:= array<EdgeFindTokenSet>
        {
            BSON_ASSERT(placeholder->sparsity >= 0 && (uint64_t)placeholder->sparsity <= (uint64_t)SIZE_MAX);
            mincover = mc_get_mincover_from_FLE2RangeFindSpec(&findSpec, (size_t)placeholder->sparsity, status);
            if (!mincover) {
                goto fail;
            }

            for (size_t i = 0; i < mc_mincover_len(mincover); i++) {
                // Create a EdgeFindTokenSet from each edge.
                bool loop_ok = false;
                const char *edge = mc_mincover_get(mincover, i);
                _mongocrypt_buffer_t edge_buf = {0};
                _FLE2EncryptedPayloadCommon_t edge_tokens = {{0}};
                mc_EdgeFindTokenSetV2_t eftc = {{0}};

                if (!_mongocrypt_buffer_from_string(&edge_buf, edge)) {
                    CLIENT_ERR("failed to copy edge to buffer");
                    goto fail_loop;
                }

                if (!_mongocrypt_fle2_placeholder_common(kb,
                                                         &edge_tokens,
                                                         &placeholder->index_key_id,
                                                         &edge_buf,
                                                         false, /* derive tokens using counter */
                                                         placeholder->maxContentionCounter,
                                                         status)) {
                    goto fail_loop;
                }

                // d := EDCDerivedToken
                _mongocrypt_buffer_steal(&eftc.edcDerivedToken, &edge_tokens.edcDerivedToken);
                // s := ESCDerivedToken
                _mongocrypt_buffer_steal(&eftc.escDerivedToken, &edge_tokens.escDerivedToken);

                // l := serverDerivedFromDataToken
                _mongocrypt_buffer_steal(&eftc.serverDerivedFromDataToken, &edge_tokens.serverDerivedFromDataToken);

                _mc_array_append_val(&payload.payload.value.edgeFindTokenSetArray, eftc);

                loop_ok = true;
            fail_loop:
                _FLE2EncryptedPayloadCommon_cleanup(&edge_tokens);
                _mongocrypt_buffer_cleanup(&edge_buf);
                if (!loop_ok) {
                    goto fail;
                }
            }
        }
        payload.payload.set = true;
    }

    payload.payloadId = findSpec.payloadId;
    payload.firstOperator = findSpec.firstOperator;
    payload.secondOperator = findSpec.secondOperator;

    // Serialize.
    {
        bson_t out = BSON_INITIALIZER;
        mc_FLE2FindRangePayloadV2_serialize(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    _mongocrypt_buffer_steal(&ciphertext->key_id, &placeholder->index_key_id);

    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2FindRangePayloadV2.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2FindRangePayloadV2;

    res = true;
fail:
    mc_mincover_destroy(mincover);
    mc_FLE2FindRangePayloadV2_cleanup(&payload);
    _mongocrypt_buffer_cleanup(&tokenKey);

    return res;
}

static bool _mongocrypt_fle2_placeholder_to_FLE2UnindexedEncryptedValue(_mongocrypt_key_broker_t *kb,
                                                                        _mongocrypt_marking_t *marking,
                                                                        _mongocrypt_ciphertext_t *ciphertext,
                                                                        mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);

    _mongocrypt_buffer_t plaintext = {0};
    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->fle2;
    _mongocrypt_buffer_t user_key = {0};
    bool res = false;

    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
    BSON_ASSERT(placeholder);
    BSON_ASSERT(placeholder->algorithm == MONGOCRYPT_FLE2_ALGORITHM_UNINDEXED);
    _mongocrypt_buffer_from_iter(&plaintext, &placeholder->v_iter);

    if (!_mongocrypt_key_broker_decrypted_key_by_id(kb, &placeholder->user_key_id, &user_key)) {
        CLIENT_ERR("unable to retrieve key");
        goto fail;
    }

    BSON_ASSERT(kb->crypt);
    if (kb->crypt->opts.use_fle2_v2) {
        res = mc_FLE2UnindexedEncryptedValueV2_encrypt(kb->crypt->crypto,
                                                       &placeholder->user_key_id,
                                                       bson_iter_type(&placeholder->v_iter),
                                                       &plaintext,
                                                       &user_key,
                                                       &ciphertext->data,
                                                       status);
        ciphertext->blob_subtype = MC_SUBTYPE_FLE2UnindexedEncryptedValueV2;
    } else {
        res = mc_FLE2UnindexedEncryptedValue_encrypt(kb->crypt->crypto,
                                                     &placeholder->user_key_id,
                                                     bson_iter_type(&placeholder->v_iter),
                                                     &plaintext,
                                                     &user_key,
                                                     &ciphertext->data,
                                                     status);
        ciphertext->blob_subtype = MC_SUBTYPE_FLE2UnindexedEncryptedValue;
    }

    if (!res) {
        goto fail;
    }

    _mongocrypt_buffer_steal(&ciphertext->key_id, &placeholder->user_key_id);
    ciphertext->original_bson_type = (uint8_t)bson_iter_type(&placeholder->v_iter);

    res = true;
fail:
    _mongocrypt_buffer_cleanup(&plaintext);
    _mongocrypt_buffer_cleanup(&user_key);

    return res;
}

static bool _mongocrypt_fle1_marking_to_ciphertext(_mongocrypt_key_broker_t *kb,
                                                   _mongocrypt_marking_t *marking,
                                                   _mongocrypt_ciphertext_t *ciphertext,
                                                   mongocrypt_status_t *status) {
    const _mongocrypt_value_encryption_algorithm_t *fle1 = _mcFLE1Algorithm();
    _mongocrypt_buffer_t plaintext;
    _mongocrypt_buffer_t iv;
    _mongocrypt_buffer_t associated_data;
    _mongocrypt_buffer_t key_material;
    _mongocrypt_buffer_t key_id;
    bool ret = false;
    bool key_found;
    uint32_t bytes_written;

    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);

    BSON_ASSERT((marking->type == MONGOCRYPT_MARKING_FLE1_BY_ID)
                || (marking->type == MONGOCRYPT_MARKING_FLE1_BY_ALTNAME));

    _mongocrypt_buffer_init(&plaintext);
    _mongocrypt_buffer_init(&associated_data);
    _mongocrypt_buffer_init(&iv);
    _mongocrypt_buffer_init(&key_id);
    _mongocrypt_buffer_init(&key_material);

    /* Get the decrypted key for this marking. */
    if (marking->type == MONGOCRYPT_MARKING_FLE1_BY_ALTNAME) {
        key_found = _mongocrypt_key_broker_decrypted_key_by_name(kb, &marking->key_alt_name, &key_material, &key_id);
    } else if (!_mongocrypt_buffer_empty(&marking->key_id)) {
        key_found = _mongocrypt_key_broker_decrypted_key_by_id(kb, &marking->key_id, &key_material);
        _mongocrypt_buffer_copy_to(&marking->key_id, &key_id);
    } else {
        CLIENT_ERR("marking must have either key_id or key_alt_name");
        goto fail;
    }

    if (!key_found) {
        _mongocrypt_status_copy_to(kb->status, status);
        goto fail;
    }

    ciphertext->original_bson_type = (uint8_t)bson_iter_type(&marking->v_iter);
    if (marking->algorithm == MONGOCRYPT_ENCRYPTION_ALGORITHM_DETERMINISTIC) {
        ciphertext->blob_subtype = MC_SUBTYPE_FLE1DeterministicEncryptedValue;
    } else {
        BSON_ASSERT(marking->algorithm == MONGOCRYPT_ENCRYPTION_ALGORITHM_RANDOM);
        ciphertext->blob_subtype = MC_SUBTYPE_FLE1RandomEncryptedValue;
    }
    _mongocrypt_buffer_copy_to(&key_id, &ciphertext->key_id);
    if (!_mongocrypt_ciphertext_serialize_associated_data(ciphertext, &associated_data)) {
        CLIENT_ERR("could not serialize associated data");
        goto fail;
    }

    _mongocrypt_buffer_from_iter(&plaintext, &marking->v_iter);
    ciphertext->data.len = fle1->get_ciphertext_len(plaintext.len, status);
    if (ciphertext->data.len == 0) {
        goto fail;
    }
    ciphertext->data.data = bson_malloc(ciphertext->data.len);
    BSON_ASSERT(ciphertext->data.data);

    ciphertext->data.owned = true;

    BSON_ASSERT(kb->crypt);
    switch (marking->algorithm) {
    case MONGOCRYPT_ENCRYPTION_ALGORITHM_DETERMINISTIC:
        /* Use deterministic encryption. */
        _mongocrypt_buffer_resize(&iv, MONGOCRYPT_IV_LEN);
        ret = _mongocrypt_calculate_deterministic_iv(kb->crypt->crypto,
                                                     &key_material,
                                                     &plaintext,
                                                     &associated_data,
                                                     &iv,
                                                     status);
        if (!ret) {
            goto fail;
        }

        ret = fle1->do_encrypt(kb->crypt->crypto,
                               &iv,
                               &associated_data,
                               &key_material,
                               &plaintext,
                               &ciphertext->data,
                               &bytes_written,
                               status);
        break;
    case MONGOCRYPT_ENCRYPTION_ALGORITHM_RANDOM:
        /* Use randomized encryption.
         * In this case, we must generate a new, random iv. */
        _mongocrypt_buffer_resize(&iv, MONGOCRYPT_IV_LEN);
        if (!_mongocrypt_random(kb->crypt->crypto, &iv, MONGOCRYPT_IV_LEN, status)) {
            goto fail;
        }
        ret = fle1->do_encrypt(kb->crypt->crypto,
                               &iv,
                               &associated_data,
                               &key_material,
                               &plaintext,
                               &ciphertext->data,
                               &bytes_written,
                               status);
        break;
    case MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE:
    default:
        /* Error. */
        CLIENT_ERR("Unsupported value for encryption algorithm");
        goto fail;
    }

    if (!ret) {
        goto fail;
    }

    BSON_ASSERT(bytes_written == ciphertext->data.len);

    ret = true;

fail:
    _mongocrypt_buffer_cleanup(&iv);
    _mongocrypt_buffer_cleanup(&key_id);
    _mongocrypt_buffer_cleanup(&plaintext);
    _mongocrypt_buffer_cleanup(&associated_data);
    _mongocrypt_buffer_cleanup(&key_material);
    return ret;
}

bool _mongocrypt_marking_to_ciphertext(void *ctx,
                                       _mongocrypt_marking_t *marking,
                                       _mongocrypt_ciphertext_t *ciphertext,
                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT_PARAM(ctx);

    _mongocrypt_key_broker_t *kb = (_mongocrypt_key_broker_t *)ctx;

    switch (marking->type) {
    case MONGOCRYPT_MARKING_FLE2_ENCRYPTION:
        switch (marking->fle2.algorithm) {
        case MONGOCRYPT_FLE2_ALGORITHM_UNINDEXED:
            return _mongocrypt_fle2_placeholder_to_FLE2UnindexedEncryptedValue(kb, marking, ciphertext, status);
        case MONGOCRYPT_FLE2_ALGORITHM_RANGE:
            switch (marking->fle2.type) {
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT:
                return _mongocrypt_fle2_placeholder_to_insert_update_ciphertextForRange(kb,
                                                                                        marking,
                                                                                        ciphertext,
                                                                                        status);
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND:
                return _mongocrypt_fle2_placeholder_to_find_ciphertextForRange(kb, marking, ciphertext, status);
            default: CLIENT_ERR("unexpected fle2 type: %d", (int)marking->fle2.type); return false;
            }
        case MONGOCRYPT_FLE2_ALGORITHM_EQUALITY:
            switch (marking->fle2.type) {
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT:
                return _mongocrypt_fle2_placeholder_to_insert_update_ciphertext(kb, marking, ciphertext, status);
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND:
                return _mongocrypt_fle2_placeholder_to_find_ciphertext(kb, marking, ciphertext, status);
            default: CLIENT_ERR("unexpected fle2 type: %d", (int)marking->fle2.type); return false;
            }
        default: CLIENT_ERR("unexpected algorithm: %d", (int)marking->algorithm); return false;
        }
    case MONGOCRYPT_MARKING_FLE1_BY_ID:
    case MONGOCRYPT_MARKING_FLE1_BY_ALTNAME:
        return _mongocrypt_fle1_marking_to_ciphertext(kb, marking, ciphertext, status);
    default: CLIENT_ERR("unexpected marking type: %d", (int)marking->type); return false;
    }
}
