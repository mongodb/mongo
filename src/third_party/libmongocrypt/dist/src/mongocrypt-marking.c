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

#include "bson/bson.h"
#include "mc-fle-blob-subtype-private.h"
#include "mc-fle2-encryption-placeholder-private.h"
#include "mc-fle2-find-equality-payload-private-v2.h"
#include "mc-fle2-find-equality-payload-private.h"
#include "mc-fle2-find-range-payload-private-v2.h"
#include "mc-fle2-find-range-payload-private.h"
#include "mc-fle2-find-text-payload-private.h"
#include "mc-fle2-insert-update-payload-private-v2.h"
#include "mc-fle2-insert-update-payload-private.h"
#include "mc-fle2-payload-uev-private.h"
#include "mc-fle2-payload-uev-v2-private.h"
#include "mc-optional-private.h"
#include "mc-range-edge-generation-private.h"
#include "mc-range-encoding-private.h"
#include "mc-range-mincover-private.h"
#include "mc-str-encode-string-sets-private.h"
#include "mc-text-search-str-encode-private.h"
#include "mc-tokens-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-ciphertext-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-key-broker-private.h"
#include "mongocrypt-marking-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-util-private.h" // mc_bson_type_to_string
#include "mongocrypt.h"

#include <math.h> // isinf

static bool
_mongocrypt_marking_parse_fle1_placeholder(const bson_t *in, _mongocrypt_marking_t *out, mongocrypt_status_t *status) {
    bson_iter_t iter = {0};
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
            if (!_mongocrypt_buffer_from_uuid_iter(&out->u.fle1.key_id, &iter)) {
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
            bson_value_copy(value, &out->u.fle1.key_alt_name);
            out->type = MONGOCRYPT_MARKING_FLE1_BY_ALTNAME;
            continue;
        }

        if (0 == strcmp("v", field)) {
            has_v = true;
            memcpy(&out->u.fle1.v_iter, &iter, sizeof(bson_iter_t));
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
            out->u.fle1.algorithm = (mongocrypt_encryption_algorithm_t)algorithm;
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
    return mc_FLE2EncryptionPlaceholder_parse(&out->u.fle2, in, status);
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
        mc_FLE2EncryptionPlaceholder_cleanup(&marking->u.fle2);
        return;
    }

    // else FLE1
    _mongocrypt_buffer_cleanup(&marking->u.fle1.key_id);
    bson_value_destroy(&marking->u.fle1.key_alt_name);
}

/**
 * Calculates:
 * E?CToken = HMAC(collectionLevel1Token, n)
 * E?CDerivedFromDataToken = HMAC(E?CToken, value)
 * E?CDerivedFromDataTokenAndContentionFactor = HMAC(E?CDerivedFromDataToken, cf)
 *
 * E?C = EDC|ESC|ECC
 * n = 1 for EDC, 2 for ESC, 3 for ECC
 * cf = contentionFactor
 *
 * If {useContentionFactor} is False, E?CDerivedFromDataToken is saved to out, and {contentionFactor} is ignored.
 * Otherwise, E?CDerivedFromDataTokenAndContentionFactor is saved to out using {contentionFactor}.
 *
 * Note that {out} is initialized even on failure.
 */
#define DERIVE_TOKEN_IMPL(Name)                                                                                        \
    static bool _fle2_derive_##Name##_token(_mongocrypt_crypto_t *crypto,                                              \
                                            _mongocrypt_buffer_t *out,                                                 \
                                            const mc_CollectionsLevel1Token_t *level1Token,                            \
                                            const _mongocrypt_buffer_t *value,                                         \
                                            bool useContentionFactor,                                                  \
                                            int64_t contentionFactor,                                                  \
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
        if (!useContentionFactor) {                                                                                    \
            /* FindEqualityPayload uses *fromDataToken */                                                              \
            _mongocrypt_buffer_copy_to(mc_##Name##DerivedFromDataToken_get(fromDataToken), out);                       \
            mc_##Name##DerivedFromDataToken_destroy(fromDataToken);                                                    \
            return true;                                                                                               \
        }                                                                                                              \
                                                                                                                       \
        BSON_ASSERT(contentionFactor >= 0);                                                                            \
        /* InsertUpdatePayload continues through *fromDataTokenAndContentionFactor */                                  \
        mc_##Name##DerivedFromDataTokenAndContentionFactor_t *fromTokenAndContentionFactor =                           \
            mc_##Name##DerivedFromDataTokenAndContentionFactor_new(crypto,                                             \
                                                                   fromDataToken,                                      \
                                                                   (uint64_t)contentionFactor,                         \
                                                                   status);                                            \
        mc_##Name##DerivedFromDataToken_destroy(fromDataToken);                                                        \
        if (!fromTokenAndContentionFactor) {                                                                           \
            return false;                                                                                              \
        }                                                                                                              \
                                                                                                                       \
        _mongocrypt_buffer_copy_to(                                                                                    \
            mc_##Name##DerivedFromDataTokenAndContentionFactor_get(fromTokenAndContentionFactor),                      \
            out);                                                                                                      \
        mc_##Name##DerivedFromDataTokenAndContentionFactor_destroy(fromTokenAndContentionFactor);                      \
                                                                                                                       \
        return true;                                                                                                   \
    }

DERIVE_TOKEN_IMPL(EDC)
DERIVE_TOKEN_IMPL(ESC)

#undef DERIVE_TOKEN_IMPL

/**
 * Calculates:
 * E?CToken = HMAC(collectionLevel1Token, n)
 * E?CText<T>Token = HMAC(E?CToken, t)
 * E?CText<T>DerivedFromDataToken = HMAC(E?CText<T>Token, v)
 * E?CText<T>DerivedFromDataTokenAndContentionFactorToken = HMAC(E?CText<T>DerivedFromDataToken, cf)
 *
 * E?C = EDC|ESC
 * n = 1 for EDC, 2 for ESC
 * <T> = Exact|Substring|Suffix|Prefix
 * t = 1 for Exact, 2 for Substring, 3 for Suffix, 4 for Prefix
 * cf = contentionFactor
 *
 * If {useContentionFactor} is False, E?CText<T>DerivedFromDataToken is saved to out, and
 * {contentionFactor} is ignored.
 * Otherwise, E?CText<T>DerivedFromDataTokenAndContentionFactorToken is saved to out.
 * Note that {out} is initialized even on failure.
 */
#define DERIVE_TEXT_SEARCH_TOKEN_IMPL(Name, Type)                                                                      \
    static bool _fle2_derive_##Name##Text##Type##_token(_mongocrypt_crypto_t *crypto,                                  \
                                                        _mongocrypt_buffer_t *out,                                     \
                                                        const mc_CollectionsLevel1Token_t *level1Token,                \
                                                        const _mongocrypt_buffer_t *value,                             \
                                                        bool useContentionFactor,                                      \
                                                        int64_t contentionFactor,                                      \
                                                        mongocrypt_status_t *status) {                                 \
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
        mc_##Name##Text##Type##Token_t *textToken = mc_##Name##Text##Type##Token_new(crypto, token, status);           \
        mc_##Name##Token_destroy(token);                                                                               \
        if (!textToken) {                                                                                              \
            return false;                                                                                              \
        }                                                                                                              \
        mc_##Name##Text##Type##DerivedFromDataToken_t *fromDataToken =                                                 \
            mc_##Name##Text##Type##DerivedFromDataToken_new(crypto, textToken, value, status);                         \
        mc_##Name##Text##Type##Token_destroy(textToken);                                                               \
        if (!fromDataToken) {                                                                                          \
            return false;                                                                                              \
        }                                                                                                              \
                                                                                                                       \
        if (!useContentionFactor) {                                                                                    \
            /* FindTextPayload uses *fromDataToken */                                                                  \
            _mongocrypt_buffer_copy_to(mc_##Name##Text##Type##DerivedFromDataToken_get(fromDataToken), out);           \
            mc_##Name##Text##Type##DerivedFromDataToken_destroy(fromDataToken);                                        \
            return true;                                                                                               \
        }                                                                                                              \
                                                                                                                       \
        BSON_ASSERT(contentionFactor >= 0);                                                                            \
        /* InsertUpdatePayload continues through *fromDataTokenAndContentionFactor */                                  \
        mc_##Name##Text##Type##DerivedFromDataTokenAndContentionFactorToken_t *fromDataAndContentionFactor =           \
            mc_##Name##Text##Type##DerivedFromDataTokenAndContentionFactorToken_new(crypto,                            \
                                                                                    fromDataToken,                     \
                                                                                    (uint64_t)contentionFactor,        \
                                                                                    status);                           \
        mc_##Name##Text##Type##DerivedFromDataToken_destroy(fromDataToken);                                            \
        if (!fromDataAndContentionFactor) {                                                                            \
            return false;                                                                                              \
        }                                                                                                              \
        _mongocrypt_buffer_copy_to(                                                                                    \
            mc_##Name##Text##Type##DerivedFromDataTokenAndContentionFactorToken_get(fromDataAndContentionFactor),      \
            out);                                                                                                      \
        mc_##Name##Text##Type##DerivedFromDataTokenAndContentionFactorToken_destroy(fromDataAndContentionFactor);      \
        return true;                                                                                                   \
    }

DERIVE_TEXT_SEARCH_TOKEN_IMPL(EDC, Exact)
DERIVE_TEXT_SEARCH_TOKEN_IMPL(ESC, Exact)
DERIVE_TEXT_SEARCH_TOKEN_IMPL(EDC, Substring)
DERIVE_TEXT_SEARCH_TOKEN_IMPL(ESC, Substring)
DERIVE_TEXT_SEARCH_TOKEN_IMPL(EDC, Suffix)
DERIVE_TEXT_SEARCH_TOKEN_IMPL(ESC, Suffix)
DERIVE_TEXT_SEARCH_TOKEN_IMPL(EDC, Prefix)
DERIVE_TEXT_SEARCH_TOKEN_IMPL(ESC, Prefix)
#undef DERIVE_TEXT_SEARCH_TOKEN_IMPL

/**
 * Calculates:
 * ServerText<T>Token = HMAC(collectionLevel1Token, t)
 * ServerText<T>DerivedFromDataToken = HMAC(ServerText<T>Token, v)
 *
 * <T> = Exact|Substring|Suffix|Prefix
 * t = 1 for Exact, 2 for Substring, 3 for Suffix, 4 for Prefix
 *
 * ServerText<T>DerivedFromDataToken is saved to out.
 * Note that {out} is initialized even on failure.
 */
#define DERIVE_TEXT_SEARCH_SERVER_DERIVED_FROM_DATA_TOKEN_IMPL(Type)                                                   \
    static bool _fle2_derive_serverText##Type##DerivedFromDataToken(                                                   \
        _mongocrypt_crypto_t *crypto,                                                                                  \
        _mongocrypt_buffer_t *out,                                                                                     \
        const mc_ServerTokenDerivationLevel1Token_t *level1Token,                                                      \
        const _mongocrypt_buffer_t *value,                                                                             \
        mongocrypt_status_t *status) {                                                                                 \
        BSON_ASSERT_PARAM(crypto);                                                                                     \
        BSON_ASSERT_PARAM(out);                                                                                        \
        BSON_ASSERT_PARAM(level1Token);                                                                                \
        BSON_ASSERT_PARAM(value);                                                                                      \
        BSON_ASSERT_PARAM(status);                                                                                     \
                                                                                                                       \
        _mongocrypt_buffer_init(out);                                                                                  \
        mc_ServerText##Type##Token_t *token = mc_ServerText##Type##Token_new(crypto, level1Token, status);             \
        if (!token) {                                                                                                  \
            return false;                                                                                              \
        }                                                                                                              \
        mc_ServerText##Type##DerivedFromDataToken_t *dataToken =                                                       \
            mc_ServerText##Type##DerivedFromDataToken_new(crypto, token, value, status);                               \
        mc_ServerText##Type##Token_destroy(token);                                                                     \
        if (!dataToken) {                                                                                              \
            return false;                                                                                              \
        }                                                                                                              \
        _mongocrypt_buffer_copy_to(mc_ServerText##Type##DerivedFromDataToken_get(dataToken), out);                     \
        mc_ServerText##Type##DerivedFromDataToken_destroy(dataToken);                                                  \
        return true;                                                                                                   \
    }

DERIVE_TEXT_SEARCH_SERVER_DERIVED_FROM_DATA_TOKEN_IMPL(Exact)
DERIVE_TEXT_SEARCH_SERVER_DERIVED_FROM_DATA_TOKEN_IMPL(Substring)
DERIVE_TEXT_SEARCH_SERVER_DERIVED_FROM_DATA_TOKEN_IMPL(Suffix)
DERIVE_TEXT_SEARCH_SERVER_DERIVED_FROM_DATA_TOKEN_IMPL(Prefix)
#undef DERIVE_TEXT_SEARCH_SERVER_DERIVED_FROM_DATA_TOKEN_IMPL

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

// FLE V1: p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor ||
//                            ECCDerivedFromDataTokenAndContentionFactor)
// FLE V2: p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor)
// Range V2: p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor || isLeaf)
// Text search: p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor || msize)
struct encrypted_token_metadata {
    mc_optional_bool_t is_leaf; // isLeaf for Range V2, none for all other cases
    mc_optional_uint32_t msize; // msize for text search, none for all other cases
};

static bool _fle2_derive_encrypted_token(_mongocrypt_crypto_t *crypto,
                                         _mongocrypt_buffer_t *out,
                                         const mc_CollectionsLevel1Token_t *collectionsLevel1Token,
                                         const _mongocrypt_buffer_t *escDerivedToken,
                                         const _mongocrypt_buffer_t *eccDerivedToken,
                                         struct encrypted_token_metadata token_metadata,
                                         mongocrypt_status_t *status) {
    // isLeaf and msize should never both be set.
    BSON_ASSERT(!token_metadata.is_leaf.set || !token_metadata.msize.set);
    mc_ECOCToken_t *ecocToken = mc_ECOCToken_new(crypto, collectionsLevel1Token, status);
    if (!ecocToken) {
        return false;
    }
    bool ok = false;

    _mongocrypt_buffer_t tmp;
    _mongocrypt_buffer_init(&tmp);
    const _mongocrypt_buffer_t *p = &tmp;
    if (!eccDerivedToken) {
        // FLE2v2
        if (token_metadata.is_leaf.set) {
            // Range V2; concat isLeaf
            _mongocrypt_buffer_t isLeafBuf;
            if (!_mongocrypt_buffer_copy_from_data_and_size(&isLeafBuf, (uint8_t[]){token_metadata.is_leaf.value}, 1)) {
                CLIENT_ERR("failed to create is_leaf buffer");
                goto fail;
            }
            if (!_mongocrypt_buffer_concat(&tmp, (_mongocrypt_buffer_t[]){*escDerivedToken, isLeafBuf}, 2)) {
                CLIENT_ERR("failed to allocate buffer");
                _mongocrypt_buffer_cleanup(&isLeafBuf);
                goto fail;
            }
            _mongocrypt_buffer_cleanup(&isLeafBuf);
        } else if (token_metadata.msize.set) {
            // Text search; concat msize
            _mongocrypt_buffer_t msizeBuf;
            // msize is a 3-byte value, so copy the 3 least significant bytes into the buffer in little-endian order.
            uint32_t le_msize = BSON_UINT32_TO_LE(token_metadata.msize.value);
            if (!_mongocrypt_buffer_copy_from_data_and_size(&msizeBuf, (uint8_t *)&le_msize, 3)) {
                CLIENT_ERR("failed to create msize buffer");
                goto fail;
            }
            if (!_mongocrypt_buffer_concat(&tmp, (_mongocrypt_buffer_t[]){*escDerivedToken, msizeBuf}, 2)) {
                CLIENT_ERR("failed to allocate buffer");
                _mongocrypt_buffer_cleanup(&msizeBuf);
                goto fail;
            }
            _mongocrypt_buffer_cleanup(&msizeBuf);
        } else {
            p = escDerivedToken;
        }

    } else {
        // FLE2v1
        const _mongocrypt_buffer_t tokens[] = {*escDerivedToken, *eccDerivedToken};
        if (!_mongocrypt_buffer_concat(&tmp, tokens, 2)) {
            CLIENT_ERR("failed to allocate buffer");
            goto fail;
        }
    }

    if (!_fle2_placeholder_aes_ctr_encrypt(crypto, mc_ECOCToken_get(ecocToken), p, out, status)) {
        goto fail;
    }

    ok = true;
fail:
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
    // Zero out memory so `_FLE2EncryptedPayloadCommon_cleanup` is safe to call twice.
    *common = (_FLE2EncryptedPayloadCommon_t){{0}};
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
        CLIENT_ERR("invalid indexKey, expected len=%d, got len=%" PRIu32, MONGOCRYPT_KEY_LEN, indexKey.len);
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
                                                bool useContentionFactor,
                                                int64_t contentionFactor,
                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(ret);
    BSON_ASSERT_PARAM(indexKeyId);
    BSON_ASSERT_PARAM(value);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
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
                                useContentionFactor,
                                contentionFactor,
                                status)) {
        goto fail;
    }

    if (!_fle2_derive_ESC_token(crypto,
                                &ret->escDerivedToken,
                                ret->collectionsLevel1Token,
                                value,
                                useContentionFactor,
                                contentionFactor,
                                status)) {
        goto fail;
    }

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

    return true;

fail:
    _FLE2EncryptedPayloadCommon_cleanup(ret);
    return false;
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
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    _mongocrypt_buffer_t value = {0};
    bool res = false;

    out->contentionFactor = 0; // k
    if (placeholder->maxContentionFactor > 0) {
        /* Choose a random contentionFactor in the inclusive range [0,
         * placeholder->maxContentionFactor] */
        if (!_mongocrypt_random_int64(crypto, placeholder->maxContentionFactor + 1, &out->contentionFactor, status)) {
            goto fail;
        }
    }

    _mongocrypt_buffer_from_iter(&value, value_iter);
    if (!_mongocrypt_fle2_placeholder_common(kb,
                                             common,
                                             &placeholder->index_key_id,
                                             &value,
                                             true, /* derive tokens using contentionFactor */
                                             out->contentionFactor,
                                             status)) {
        goto fail;
    }

    // d := EDCDerivedToken
    _mongocrypt_buffer_steal(&out->edcDerivedToken, &common->edcDerivedToken);
    // s := ESCDerivedToken
    _mongocrypt_buffer_steal(&out->escDerivedToken, &common->escDerivedToken);
    BSON_ASSERT(common->eccDerivedToken.data == NULL);

    // p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor)
    // Or in Range V2, when using range: p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor || 0x00)
    // Or in Text Search, when using msize: p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor ||
    // 0x000000)
    struct encrypted_token_metadata et_meta = {{0}};
    if (placeholder->algorithm == MONGOCRYPT_FLE2_ALGORITHM_RANGE) {
        // For range, we append isLeaf to the encryptedTokens.
        et_meta.is_leaf = OPT_BOOL(false);
    } else if (placeholder->algorithm == MONGOCRYPT_FLE2_ALGORITHM_TEXT_SEARCH) {
        // For text search, we append msize to the encryptedTokens.
        et_meta.msize = OPT_U32(0);
    }
    if (!_fle2_derive_encrypted_token(crypto,
                                      &out->encryptedTokens,
                                      common->collectionsLevel1Token,
                                      &out->escDerivedToken,
                                      NULL, // unused in v2
                                      et_meta,
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

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->u.fle2;
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
                                                          .sparsity = sparsity,
                                                          .trimFactor = insertSpec->trimFactor},
                                status);
    }

    else if (value_type == BSON_TYPE_INT64) {
        return mc_getEdgesInt64((mc_getEdgesInt64_args_t){.value = bson_iter_int64(&insertSpec->v),
                                                          .min = OPT_I64(bson_iter_int64(&insertSpec->min)),
                                                          .max = OPT_I64(bson_iter_int64(&insertSpec->max)),
                                                          .sparsity = sparsity,
                                                          .trimFactor = insertSpec->trimFactor},
                                status);
    }

    else if (value_type == BSON_TYPE_DATE_TIME) {
        return mc_getEdgesInt64((mc_getEdgesInt64_args_t){.value = bson_iter_date_time(&insertSpec->v),
                                                          .min = OPT_I64(bson_iter_date_time(&insertSpec->min)),
                                                          .max = OPT_I64(bson_iter_date_time(&insertSpec->max)),
                                                          .sparsity = sparsity,
                                                          .trimFactor = insertSpec->trimFactor},
                                status);
    }

    else if (value_type == BSON_TYPE_DOUBLE) {
        mc_getEdgesDouble_args_t args = {.value = bson_iter_double(&insertSpec->v),
                                         .sparsity = sparsity,
                                         .trimFactor = insertSpec->trimFactor};
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
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT()
        const mc_dec128 value = mc_dec128_from_bson_iter(&insertSpec->v);
        mc_getEdgesDecimal128_args_t args = {
            .value = value,
            .sparsity = sparsity,
            .trimFactor = insertSpec->trimFactor,
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
 * Payload subtype 11: FLE2InsertUpdatePayloadV2 for range updates
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

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->u.fle2;
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
                                                              &marking->u.fle2,
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
            bool is_leaf = mc_edges_is_leaf(edges, edge);
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
                                                     true, /* derive tokens using contentionFactor */
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

            // p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor)
            // Or in Range V2: p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor || isLeaf)
            if (!_fle2_derive_encrypted_token(kb->crypt->crypto,
                                              &etc.encryptedTokens,
                                              edge_tokens.collectionsLevel1Token,
                                              &etc.escDerivedToken,
                                              NULL, // ecc unsed in FLE2v2
                                              (struct encrypted_token_metadata){.is_leaf = OPT_BOOL(is_leaf)},
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

    // Include "range" payload fields introduced in SERVER-91889.
    payload.sparsity = OPT_I64(placeholder->sparsity);
    payload.precision = insertSpec.precision;
    payload.trimFactor = OPT_I32(mc_edges_get_used_trimFactor(edges));
    bson_value_copy(bson_iter_value(&insertSpec.min), &payload.indexMin);
    bson_value_copy(bson_iter_value(&insertSpec.max), &payload.indexMax);

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
 * Sets up a mc_Text<T>TokenSet_t type by generating its member tokens:
 * - edcDerivedToken = HMAC(HMAC(HMAC(EDCToken, t), v), cf)
 * - escDerivedToken = HMAC(HMAC(HMAC(ESCToken, t), v), cf)
 * - serverDerivedFromDataToken = HMAC(HMAC(ServerLevel1Token, t), v)
 * and the encrypted token:
 * - encryptedTokens = EncryptCTR(ECOCToken, escDerivedToken)
 *
 * <T> = Exact|Substring|Suffix|Prefix
 * t = 1 for Exact, 2 for Substring, 3 for Suffix, 4 for Prefix
 * cf = contentionFactor
 * EDC/ESC/ECOCToken are derived from {collLevel1Token}
 */
#define GENERATE_TEXT_SEARCH_TOKEN_SET_FOR_TYPE_IMPL(Type)                                                             \
    static bool _fle2_generate_Text##Type##TokenSet(_mongocrypt_key_broker_t *kb,                                      \
                                                    mc_Text##Type##TokenSet_t *out,                                    \
                                                    const _mongocrypt_buffer_t *value,                                 \
                                                    int64_t contentionFactor,                                          \
                                                    uint32_t msize,                                                    \
                                                    const mc_CollectionsLevel1Token_t *collLevel1Token,                \
                                                    const mc_ServerTokenDerivationLevel1Token_t *serverLevel1Token,    \
                                                    mongocrypt_status_t *status) {                                     \
        BSON_ASSERT_PARAM(kb);                                                                                         \
        BSON_ASSERT_PARAM(kb->crypt);                                                                                  \
        BSON_ASSERT_PARAM(out);                                                                                        \
        BSON_ASSERT_PARAM(value);                                                                                      \
        BSON_ASSERT_PARAM(collLevel1Token);                                                                            \
        BSON_ASSERT_PARAM(serverLevel1Token);                                                                          \
                                                                                                                       \
        if (!_fle2_derive_EDCText##Type##_token(kb->crypt->crypto,                                                     \
                                                &out->edcDerivedToken,                                                 \
                                                collLevel1Token,                                                       \
                                                value,                                                                 \
                                                true,                                                                  \
                                                contentionFactor,                                                      \
                                                status)) {                                                             \
            return false;                                                                                              \
        }                                                                                                              \
        if (!_fle2_derive_ESCText##Type##_token(kb->crypt->crypto,                                                     \
                                                &out->escDerivedToken,                                                 \
                                                collLevel1Token,                                                       \
                                                value,                                                                 \
                                                true,                                                                  \
                                                contentionFactor,                                                      \
                                                status)) {                                                             \
            return false;                                                                                              \
        }                                                                                                              \
        if (!_fle2_derive_serverText##Type##DerivedFromDataToken(kb->crypt->crypto,                                    \
                                                                 &out->serverDerivedFromDataToken,                     \
                                                                 serverLevel1Token,                                    \
                                                                 value,                                                \
                                                                 status)) {                                            \
            return false;                                                                                              \
        }                                                                                                              \
        if (!_fle2_derive_encrypted_token(kb->crypt->crypto,                                                           \
                                          &out->encryptedTokens,                                                       \
                                          collLevel1Token,                                                             \
                                          &out->escDerivedToken,                                                       \
                                          NULL,                                                                        \
                                          (struct encrypted_token_metadata){.msize = OPT_U32(msize)},                  \
                                          status)) {                                                                   \
            return false;                                                                                              \
        }                                                                                                              \
        return true;                                                                                                   \
    }                                                                                                                  \
    static bool _fle2_generate_Text##Type##FindTokenSet(                                                               \
        _mongocrypt_key_broker_t *kb,                                                                                  \
        mc_Text##Type##FindTokenSet_t *out,                                                                            \
        const _mongocrypt_buffer_t *value,                                                                             \
        const mc_CollectionsLevel1Token_t *collLevel1Token,                                                            \
        const mc_ServerTokenDerivationLevel1Token_t *serverLevel1Token,                                                \
        mongocrypt_status_t *status) {                                                                                 \
        BSON_ASSERT_PARAM(kb);                                                                                         \
        BSON_ASSERT_PARAM(kb->crypt);                                                                                  \
        BSON_ASSERT_PARAM(out);                                                                                        \
        BSON_ASSERT_PARAM(value);                                                                                      \
        BSON_ASSERT_PARAM(collLevel1Token);                                                                            \
        BSON_ASSERT_PARAM(serverLevel1Token);                                                                          \
        if (!_fle2_derive_EDCText##Type##_token(kb->crypt->crypto,                                                     \
                                                &out->edcDerivedToken,                                                 \
                                                collLevel1Token,                                                       \
                                                value,                                                                 \
                                                false,                                                                 \
                                                0,                                                                     \
                                                status)) {                                                             \
            return false;                                                                                              \
        }                                                                                                              \
        if (!_fle2_derive_ESCText##Type##_token(kb->crypt->crypto,                                                     \
                                                &out->escDerivedToken,                                                 \
                                                collLevel1Token,                                                       \
                                                value,                                                                 \
                                                false,                                                                 \
                                                0,                                                                     \
                                                status)) {                                                             \
            return false;                                                                                              \
        }                                                                                                              \
        if (!_fle2_derive_serverText##Type##DerivedFromDataToken(kb->crypt->crypto,                                    \
                                                                 &out->serverDerivedFromDataToken,                     \
                                                                 serverLevel1Token,                                    \
                                                                 value,                                                \
                                                                 status)) {                                            \
            return false;                                                                                              \
        }                                                                                                              \
        return true;                                                                                                   \
    }
GENERATE_TEXT_SEARCH_TOKEN_SET_FOR_TYPE_IMPL(Exact)
GENERATE_TEXT_SEARCH_TOKEN_SET_FOR_TYPE_IMPL(Substring)
GENERATE_TEXT_SEARCH_TOKEN_SET_FOR_TYPE_IMPL(Suffix)
GENERATE_TEXT_SEARCH_TOKEN_SET_FOR_TYPE_IMPL(Prefix)
#undef GENERATE_TEXT_SEARCH_TOKEN_SET_FOR_TYPE_IMPL

static bool _fle2_generate_TextSearchTokenSets(_mongocrypt_key_broker_t *kb,
                                               mc_FLE2InsertUpdatePayloadV2_t *payload,
                                               const _mongocrypt_buffer_t *indexKeyId,
                                               const mc_FLE2TextSearchInsertSpec_t *spec,
                                               int64_t contentionFactor,
                                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(payload);
    BSON_ASSERT_PARAM(indexKeyId);
    BSON_ASSERT_PARAM(spec);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    mc_TextSearchTokenSets_t *tsts = &payload->textSearchTokenSets.tsts;
    _FLE2EncryptedPayloadCommon_t common = {{0}};
    bool res = false;

    mc_str_encode_sets_t *encodeSets = mc_text_search_str_encode(spec, status);
    if (!encodeSets) {
        goto fail;
    }

    // Start the token derivations
    if (!_get_tokenKey(kb, indexKeyId, &common.tokenKey, status)) {
        goto fail;
    }

    common.collectionsLevel1Token = mc_CollectionsLevel1Token_new(crypto, &common.tokenKey, status);
    if (!common.collectionsLevel1Token) {
        CLIENT_ERR("unable to derive collectionLevel1Token");
        goto fail;
    }

    common.serverTokenDerivationLevel1Token = mc_ServerTokenDerivationLevel1Token_new(crypto, &common.tokenKey, status);
    if (!common.serverTokenDerivationLevel1Token) {
        CLIENT_ERR("unable to derive serverTokenDerivationLevel1Token");
        goto fail;
    }

    // Generate exact token set singleton
    {
        _mongocrypt_buffer_t asBsonValue;
        _mongocrypt_buffer_init(&asBsonValue);
        BSON_ASSERT(encodeSets->exact.len < INT_MAX);
        _mongocrypt_buffer_copy_from_string_as_bson_value(&asBsonValue,
                                                          (const char *)encodeSets->exact.data,
                                                          (int)encodeSets->exact.len);
        if (!_fle2_generate_TextExactTokenSet(kb,
                                              &tsts->exact,
                                              &asBsonValue,
                                              contentionFactor,
                                              // For the exact token, report total msize of the token set.
                                              encodeSets->msize,
                                              common.collectionsLevel1Token,
                                              common.serverTokenDerivationLevel1Token,
                                              status)) {
            _mongocrypt_buffer_cleanup(&asBsonValue);
            goto fail;
        }
        _mongocrypt_buffer_cleanup(&asBsonValue);
    }

    const char *substring;
    uint32_t bytelen;
    uint32_t appendCount;

    // Generate array of substring token sets
    if (encodeSets->substring_set) {
        mc_substring_set_iter_t set_itr;
        mc_substring_set_iter_init(&set_itr, encodeSets->substring_set);

        while (mc_substring_set_iter_next(&set_itr, &substring, &bytelen, &appendCount)) {
            BSON_ASSERT(appendCount > 0);
            BSON_ASSERT(bytelen < INT_MAX);

            mc_TextSubstringTokenSet_t tset = {{0}};

            _mongocrypt_buffer_t asBsonValue;
            _mongocrypt_buffer_init(&asBsonValue);
            _mongocrypt_buffer_copy_from_string_as_bson_value(&asBsonValue, substring, (int)bytelen);

            // For substring, prefix, and suffix tokens, report 0 as the msize.
            if (!_fle2_generate_TextSubstringTokenSet(kb,
                                                      &tset,
                                                      &asBsonValue,
                                                      contentionFactor,
                                                      0 /* msize */,
                                                      common.collectionsLevel1Token,
                                                      common.serverTokenDerivationLevel1Token,
                                                      status)) {
                _mongocrypt_buffer_cleanup(&asBsonValue);
                mc_TextSubstringTokenSet_cleanup(&tset);
                goto fail;
            }
            _mongocrypt_buffer_cleanup(&asBsonValue);

            if (appendCount > 1) {
                mc_TextSubstringTokenSet_t tset_copy;
                mc_TextSubstringTokenSet_shallow_copy(&tset, &tset_copy);
                for (; appendCount > 1; appendCount--) {
                    _mc_array_append_val(&tsts->substringArray, tset_copy);
                }
            }
            _mc_array_append_val(&tsts->substringArray, tset); // array now owns tset
        }
    }

    // Generate array of suffix token sets
    if (encodeSets->suffix_set) {
        mc_affix_set_iter_t set_itr;
        mc_affix_set_iter_init(&set_itr, encodeSets->suffix_set);

        while (mc_affix_set_iter_next(&set_itr, &substring, &bytelen, &appendCount)) {
            BSON_ASSERT(appendCount > 0);
            BSON_ASSERT(bytelen < INT_MAX);

            mc_TextSuffixTokenSet_t tset = {{0}};
            mc_TextSuffixTokenSet_init(&tset);

            _mongocrypt_buffer_t asBsonValue;
            _mongocrypt_buffer_init(&asBsonValue);
            _mongocrypt_buffer_copy_from_string_as_bson_value(&asBsonValue, substring, (int)bytelen);

            if (!_fle2_generate_TextSuffixTokenSet(kb,
                                                   &tset,
                                                   &asBsonValue,
                                                   contentionFactor,
                                                   0 /* msize */,
                                                   common.collectionsLevel1Token,
                                                   common.serverTokenDerivationLevel1Token,
                                                   status)) {
                _mongocrypt_buffer_cleanup(&asBsonValue);
                mc_TextSuffixTokenSet_cleanup(&tset);
                goto fail;
            }
            _mongocrypt_buffer_cleanup(&asBsonValue);

            if (appendCount > 1) {
                mc_TextSuffixTokenSet_t tset_copy;
                mc_TextSuffixTokenSet_shallow_copy(&tset, &tset_copy);
                for (; appendCount > 1; appendCount--) {
                    _mc_array_append_val(&tsts->suffixArray, tset_copy);
                }
            }
            _mc_array_append_val(&tsts->suffixArray, tset); // array now owns tset
        }
    }

    // Generate array of prefix token sets
    if (encodeSets->prefix_set) {
        mc_affix_set_iter_t set_itr;
        mc_affix_set_iter_init(&set_itr, encodeSets->prefix_set);

        while (mc_affix_set_iter_next(&set_itr, &substring, &bytelen, &appendCount)) {
            BSON_ASSERT(appendCount > 0);
            BSON_ASSERT(bytelen < INT_MAX);

            mc_TextPrefixTokenSet_t tset = {{0}};
            mc_TextPrefixTokenSet_init(&tset);

            _mongocrypt_buffer_t asBsonValue;
            _mongocrypt_buffer_init(&asBsonValue);
            _mongocrypt_buffer_copy_from_string_as_bson_value(&asBsonValue, substring, (int)bytelen);

            if (!_fle2_generate_TextPrefixTokenSet(kb,
                                                   &tset,
                                                   &asBsonValue,
                                                   contentionFactor,
                                                   0 /* msize */,
                                                   common.collectionsLevel1Token,
                                                   common.serverTokenDerivationLevel1Token,
                                                   status)) {
                _mongocrypt_buffer_cleanup(&asBsonValue);
                mc_TextPrefixTokenSet_cleanup(&tset);
                goto fail;
            }
            _mongocrypt_buffer_cleanup(&asBsonValue);

            if (appendCount > 1) {
                mc_TextPrefixTokenSet_t tset_copy;
                mc_TextPrefixTokenSet_shallow_copy(&tset, &tset_copy);
                for (; appendCount > 1; appendCount--) {
                    _mc_array_append_val(&tsts->prefixArray, tset_copy); // array now owns tset_copy
                }
            }
            _mc_array_append_val(&tsts->prefixArray, tset); // moves ownership of tset
        }
    }
    payload->textSearchTokenSets.set = true;
    res = true;
fail:
    _FLE2EncryptedPayloadCommon_cleanup(&common);
    mc_str_encode_sets_destroy(encodeSets);
    return res;
}

static bool _fle2_generate_TextSearchFindTokenSets(_mongocrypt_key_broker_t *kb,
                                                   mc_TextSearchFindTokenSets_t *out,
                                                   const _mongocrypt_buffer_t *indexKeyId,
                                                   const mc_FLE2TextSearchInsertSpec_t *spec,
                                                   mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(kb->crypt);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(indexKeyId);
    BSON_ASSERT_PARAM(spec);

    _mongocrypt_crypto_t *crypto = kb->crypt->crypto;
    _FLE2EncryptedPayloadCommon_t common = {{0}};
    _mongocrypt_buffer_t asBsonValue = {0};
    bool res = false;

    int operator_count = (int)spec->substr.set + (int)spec->suffix.set + (int)spec->prefix.set;
    if (operator_count > 1) {
        CLIENT_ERR("Text search query specification cannot contain multiple query type specifications");
        goto fail;
    }

    if (!mc_text_search_str_query(spec, &asBsonValue, status)) {
        goto fail;
    }

    // Start the token derivations
    if (!_get_tokenKey(kb, indexKeyId, &common.tokenKey, status)) {
        goto fail;
    }

    common.collectionsLevel1Token = mc_CollectionsLevel1Token_new(crypto, &common.tokenKey, status);
    if (!common.collectionsLevel1Token) {
        CLIENT_ERR("unable to derive collectionLevel1Token");
        goto fail;
    }

    common.serverTokenDerivationLevel1Token = mc_ServerTokenDerivationLevel1Token_new(crypto, &common.tokenKey, status);
    if (!common.serverTokenDerivationLevel1Token) {
        CLIENT_ERR("unable to derive serverTokenDerivationLevel1Token");
        goto fail;
    }

    if (spec->substr.set) {
        if (!_fle2_generate_TextSubstringFindTokenSet(kb,
                                                      &out->substring.value,
                                                      &asBsonValue,
                                                      common.collectionsLevel1Token,
                                                      common.serverTokenDerivationLevel1Token,
                                                      status)) {
            goto fail;
        }
        out->substring.set = true;
    } else if (spec->suffix.set) {
        if (!_fle2_generate_TextSuffixFindTokenSet(kb,
                                                   &out->suffix.value,
                                                   &asBsonValue,
                                                   common.collectionsLevel1Token,
                                                   common.serverTokenDerivationLevel1Token,
                                                   status)) {
            goto fail;
        }
        out->suffix.set = true;

    } else if (spec->prefix.set) {
        if (!_fle2_generate_TextPrefixFindTokenSet(kb,
                                                   &out->prefix.value,
                                                   &asBsonValue,
                                                   common.collectionsLevel1Token,
                                                   common.serverTokenDerivationLevel1Token,
                                                   status)) {
            goto fail;
        }
        out->prefix.set = true;
    } else {
        if (!_fle2_generate_TextExactFindTokenSet(kb,
                                                  &out->exact.value,
                                                  &asBsonValue,
                                                  common.collectionsLevel1Token,
                                                  common.serverTokenDerivationLevel1Token,
                                                  status)) {
            goto fail;
        }
        out->exact.set = true;
    }
    res = true;
fail:
    _mongocrypt_buffer_cleanup(&asBsonValue);
    _FLE2EncryptedPayloadCommon_cleanup(&common);
    return res;
}

/**
 * Payload subtype 11: FLE2InsertUpdatePayloadV2 for text search inserts/updates
 *
 * {v: value, u: indexKeyId, t: valueType, k: contentionFactor, e: serverToken,
 *  b: { e: {d: EDC_exact, s: ESC_exact, l: svrDFDToken_exact, p: encToken_exact},
 *       s: [{d: EDC_substr, s: ESC_substr, l: svrDFDToken_substr, p: encToken_substr}, ...]
 *       u: [{d: EDC_suffix, s: ESC_suffix, l: svrDFDToken_suffix, p: encToken_suffix}, ...]
 *       p: [{d: EDC_prefix, s: ESC_prefix, l: svrDFDToken_prefix, p: encToken_prefix}, ...]
 *     },
 *  d: bogusToken, s: bogusToken, l: bogusToken, p: bogusCiphertext
 * }
 */
static bool _mongocrypt_fle2_placeholder_to_insert_update_ciphertextForTextSearch(_mongocrypt_key_broker_t *kb,
                                                                                  _mongocrypt_marking_t *marking,
                                                                                  _mongocrypt_ciphertext_t *ciphertext,
                                                                                  mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->u.fle2;
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT);
    BSON_ASSERT(placeholder->algorithm == MONGOCRYPT_FLE2_ALGORITHM_TEXT_SEARCH);

    _FLE2EncryptedPayloadCommon_t common = {{0}};
    mc_FLE2InsertUpdatePayloadV2_t payload;
    mc_FLE2InsertUpdatePayloadV2_init(&payload);

    _mongocrypt_buffer_t value = {0};
    bool res = false;

    mc_FLE2TextSearchInsertSpec_t insertSpec;
    if (!mc_FLE2TextSearchInsertSpec_parse(&insertSpec, &placeholder->v_iter, status)) {
        goto fail;
    }

    // One of substr/suffix/prefix must be set for inserts
    if (!(insertSpec.substr.set || insertSpec.suffix.set || insertSpec.prefix.set)) {
        CLIENT_ERR("FLE2TextSearchInsertSpec is missing a substring, suffix, or prefix index specification");
        goto fail;
    }

    // t
    payload.valueType = BSON_TYPE_UTF8;

    // k
    payload.contentionFactor = 0;
    if (placeholder->maxContentionFactor > 0) {
        /* Choose a random contentionFactor in the inclusive range [0,
         * placeholder->maxContentionFactor] */
        if (!_mongocrypt_random_int64(kb->crypt->crypto,
                                      placeholder->maxContentionFactor + 1,
                                      &payload.contentionFactor,
                                      status)) {
            goto fail;
        }
    }

    // u
    _mongocrypt_buffer_copy_to(&placeholder->index_key_id, &payload.indexKeyId);

    _mongocrypt_buffer_from_iter(&value, &insertSpec.v_iter);
    if (!_mongocrypt_fle2_placeholder_common(kb,
                                             &common,
                                             &placeholder->index_key_id,
                                             &value,
                                             true, /* derive tokens using contentionFactor */
                                             payload.contentionFactor,
                                             status)) {
        goto fail;
    }

    // (d, s, l) are never used for text search, so just set these to bogus buffers of
    // correct length.
    BSON_ASSERT(_mongocrypt_buffer_steal_from_data_and_size(&payload.edcDerivedToken,
                                                            bson_malloc0(MONGOCRYPT_HMAC_SHA256_LEN),
                                                            MONGOCRYPT_HMAC_SHA256_LEN));
    _mongocrypt_buffer_copy_to(&payload.edcDerivedToken, &payload.escDerivedToken);
    _mongocrypt_buffer_copy_to(&payload.edcDerivedToken, &payload.serverDerivedFromDataToken);

    // p := EncryptCTR(ECOCToken, ESCDerivedFromDataTokenAndContentionFactor | 0x000000)
    // Since p is never used for text search, this just sets p to a bogus ciphertext of
    // the correct length.
    if (!_fle2_derive_encrypted_token(kb->crypt->crypto,
                                      &payload.encryptedTokens,
                                      common.collectionsLevel1Token,
                                      &payload.escDerivedToken, // bogus
                                      NULL,                     // unused in FLE2v2
                                      (struct encrypted_token_metadata){.msize = OPT_U32(0)},
                                      status)) {
        goto fail;
    }

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
        const bool ok = _mongocrypt_buffer_concat(&payload.value, v, 2);
        _mongocrypt_buffer_cleanup(&ciphertext);
        if (!ok) {
            goto fail;
        }
    }
    // e := ServerDataEncryptionLevel1Token
    _mongocrypt_buffer_copy_to(mc_ServerDataEncryptionLevel1Token_get(common.serverDataEncryptionLevel1Token),
                               &payload.serverEncryptionToken);

    // b
    if (!_fle2_generate_TextSearchTokenSets(kb,
                                            &payload,
                                            &placeholder->index_key_id,
                                            &insertSpec,
                                            payload.contentionFactor,
                                            status)) {
        goto fail;
    }

    {
        bson_t out;
        bson_init(&out);
        mc_FLE2InsertUpdatePayloadV2_serializeForTextSearch(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }
    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2InsertUpdatePayloadV2.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2InsertUpdatePayloadV2;

    res = true;
fail:
    mc_FLE2InsertUpdatePayloadV2_cleanup(&payload);
    _mongocrypt_buffer_cleanup(&value);
    _FLE2EncryptedPayloadCommon_cleanup(&common);
    return res;
}

/**
 * Payload subtype 12: FLE2FindEqualityPayloadV2
 *
 * {d: EDC, s: ESC, l: serverDerivedFromDataToken, cm: maxContentionFactor}
 */
static bool _mongocrypt_fle2_placeholder_to_find_ciphertext(_mongocrypt_key_broker_t *kb,
                                                            _mongocrypt_marking_t *marking,
                                                            _mongocrypt_ciphertext_t *ciphertext,
                                                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);

    _FLE2EncryptedPayloadCommon_t common = {{0}};
    _mongocrypt_buffer_t value = {0};
    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->u.fle2;
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
                                             false, /* derive tokens without contentionFactor */
                                             placeholder->maxContentionFactor, /* ignored */
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

    // cm := maxContentionFactor
    payload.maxContentionFactor = placeholder->maxContentionFactor;

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
                                         .sparsity = sparsity,
                                         .trimFactor = findSpec->edgesInfo.value.trimFactor},
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
                                         .sparsity = sparsity,
                                         .trimFactor = findSpec->edgesInfo.value.trimFactor},
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
                                         .sparsity = sparsity,
                                         .trimFactor = findSpec->edgesInfo.value.trimFactor},
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
                                            .sparsity = sparsity,
                                            .trimFactor = findSpec->edgesInfo.value.trimFactor};
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
#if MONGOCRYPT_HAVE_DECIMAL128_SUPPORT()
        BSON_ASSERT(bson_iter_type(&lowerBound) == BSON_TYPE_DECIMAL128);
        BSON_ASSERT(bson_iter_type(&upperBound) == BSON_TYPE_DECIMAL128);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMin) == BSON_TYPE_DECIMAL128);
        BSON_ASSERT(bson_iter_type(&findSpec->edgesInfo.value.indexMax) == BSON_TYPE_DECIMAL128);

        mc_getMincoverDecimal128_args_t args = {.lowerBound = mc_dec128_from_bson_iter(&lowerBound),
                                                .includeLowerBound = includeLowerBound,
                                                .upperBound = mc_dec128_from_bson_iter(&upperBound),
                                                .includeUpperBound = includeUpperBound,
                                                .sparsity = sparsity,
                                                .trimFactor = findSpec->edgesInfo.value.trimFactor};
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
 * Payload subtype 13: FLE2FindRangePayloadV2
 *
 * {cm: maxContentionFactor,
 *  g: [{d: EDC, s: ESC, l: serverDerivedFromDataToken}, ...]}
 */
static bool _mongocrypt_fle2_placeholder_to_find_ciphertextForRange(_mongocrypt_key_broker_t *kb,
                                                                    _mongocrypt_marking_t *marking,
                                                                    _mongocrypt_ciphertext_t *ciphertext,
                                                                    mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);

    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->u.fle2;
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
        // cm := Queryable Encryption max contentionFactor
        payload.payload.value.maxContentionFactor = placeholder->maxContentionFactor;

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
                                                         false, /* derive tokens without using contentionFactor */
                                                         placeholder->maxContentionFactor, /* ignored */
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

        // Include "range" payload fields introduced in SERVER-91889.
        payload.sparsity = OPT_I64(placeholder->sparsity);
        payload.precision = findSpec.edgesInfo.value.precision;
        payload.trimFactor = OPT_I32(mc_mincover_get_used_trimFactor(mincover));
        bson_value_copy(bson_iter_value(&findSpec.edgesInfo.value.indexMin), &payload.indexMin);
        bson_value_copy(bson_iter_value(&findSpec.edgesInfo.value.indexMax), &payload.indexMax);
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

static bool _mongocrypt_fle2_placeholder_to_find_ciphertextForTextSearch(_mongocrypt_key_broker_t *kb,
                                                                         _mongocrypt_marking_t *marking,
                                                                         _mongocrypt_ciphertext_t *ciphertext,
                                                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(ciphertext);
    BSON_ASSERT(kb->crypt);
    BSON_ASSERT(marking->type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);

    bool res = false;
    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->u.fle2;
    BSON_ASSERT(placeholder->type == MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND);
    BSON_ASSERT(placeholder->algorithm == MONGOCRYPT_FLE2_ALGORITHM_TEXT_SEARCH);

    mc_FLE2FindTextPayload_t payload;
    mc_FLE2FindTextPayload_init(&payload);

    mc_FLE2TextSearchInsertSpec_t spec;
    if (!mc_FLE2TextSearchInsertSpec_parse(&spec, &placeholder->v_iter, status)) {
        goto fail;
    }

    if (!_fle2_generate_TextSearchFindTokenSets(kb, &payload.tokenSets, &placeholder->index_key_id, &spec, status)) {
        goto fail;
    }

    payload.caseFold = spec.casef;
    payload.diacriticFold = spec.diacf;
    payload.maxContentionFactor = placeholder->maxContentionFactor;
    if (spec.substr.set) {
        payload.substringSpec.set = true;
        payload.substringSpec.value = spec.substr.value;
    } else if (spec.suffix.set) {
        payload.suffixSpec.set = true;
        payload.suffixSpec.value = spec.suffix.value;
    } else if (spec.prefix.set) {
        payload.prefixSpec.set = true;
        payload.prefixSpec.value = spec.prefix.value;
    }

    // Serialize.
    {
        bson_t out = BSON_INITIALIZER;
        mc_FLE2FindTextPayload_serialize(&payload, &out);
        _mongocrypt_buffer_steal_from_bson(&ciphertext->data, &out);
    }

    // Do not set ciphertext->original_bson_type and ciphertext->key_id. They are
    // not used for FLE2FindTextPayload.
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2FindTextPayload;

    res = true;

fail:
    mc_FLE2FindTextPayload_cleanup(&payload);
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
    mc_FLE2EncryptionPlaceholder_t *placeholder = &marking->u.fle2;
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
    res = mc_FLE2UnindexedEncryptedValueV2_encrypt(kb->crypt->crypto,
                                                   &placeholder->user_key_id,
                                                   bson_iter_type(&placeholder->v_iter),
                                                   &plaintext,
                                                   &user_key,
                                                   &ciphertext->data,
                                                   status);
    ciphertext->blob_subtype = MC_SUBTYPE_FLE2UnindexedEncryptedValueV2;

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

    /* Get the decrypted key for this marking.u.fle1. */
    if (marking->type == MONGOCRYPT_MARKING_FLE1_BY_ALTNAME) {
        key_found =
            _mongocrypt_key_broker_decrypted_key_by_name(kb, &marking->u.fle1.key_alt_name, &key_material, &key_id);
    } else if (!_mongocrypt_buffer_empty(&marking->u.fle1.key_id)) {
        key_found = _mongocrypt_key_broker_decrypted_key_by_id(kb, &marking->u.fle1.key_id, &key_material);
        _mongocrypt_buffer_copy_to(&marking->u.fle1.key_id, &key_id);
    } else {
        CLIENT_ERR("marking must have either key_id or key_alt_name");
        goto fail;
    }

    if (!key_found) {
        _mongocrypt_status_copy_to(kb->status, status);
        goto fail;
    }

    ciphertext->original_bson_type = (uint8_t)bson_iter_type(&marking->u.fle1.v_iter);
    if (marking->u.fle1.algorithm == MONGOCRYPT_ENCRYPTION_ALGORITHM_DETERMINISTIC) {
        ciphertext->blob_subtype = MC_SUBTYPE_FLE1DeterministicEncryptedValue;
    } else {
        BSON_ASSERT(marking->u.fle1.algorithm == MONGOCRYPT_ENCRYPTION_ALGORITHM_RANDOM);
        ciphertext->blob_subtype = MC_SUBTYPE_FLE1RandomEncryptedValue;
    }
    _mongocrypt_buffer_copy_to(&key_id, &ciphertext->key_id);
    if (!_mongocrypt_ciphertext_serialize_associated_data(ciphertext, &associated_data)) {
        CLIENT_ERR("could not serialize associated data");
        goto fail;
    }

    _mongocrypt_buffer_from_iter(&plaintext, &marking->u.fle1.v_iter);
    ciphertext->data.len = fle1->get_ciphertext_len(plaintext.len, status);
    if (ciphertext->data.len == 0) {
        goto fail;
    }
    ciphertext->data.data = bson_malloc(ciphertext->data.len);
    BSON_ASSERT(ciphertext->data.data);

    ciphertext->data.owned = true;

    BSON_ASSERT(kb->crypt);
    switch (marking->u.fle1.algorithm) {
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
        switch (marking->u.fle2.algorithm) {
        case MONGOCRYPT_FLE2_ALGORITHM_UNINDEXED:
            return _mongocrypt_fle2_placeholder_to_FLE2UnindexedEncryptedValue(kb, marking, ciphertext, status);
        case MONGOCRYPT_FLE2_ALGORITHM_RANGE:
            switch (marking->u.fle2.type) {
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT:
                return _mongocrypt_fle2_placeholder_to_insert_update_ciphertextForRange(kb,
                                                                                        marking,
                                                                                        ciphertext,
                                                                                        status);
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND:
                return _mongocrypt_fle2_placeholder_to_find_ciphertextForRange(kb, marking, ciphertext, status);
            default: CLIENT_ERR("unexpected fle2 type: %d", (int)marking->u.fle2.type); return false;
            }
        case MONGOCRYPT_FLE2_ALGORITHM_EQUALITY:
            switch (marking->u.fle2.type) {
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT:
                return _mongocrypt_fle2_placeholder_to_insert_update_ciphertext(kb, marking, ciphertext, status);
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND:
                return _mongocrypt_fle2_placeholder_to_find_ciphertext(kb, marking, ciphertext, status);
            default: CLIENT_ERR("unexpected fle2 type: %d", (int)marking->u.fle2.type); return false;
            }
        case MONGOCRYPT_FLE2_ALGORITHM_TEXT_SEARCH:
            switch (marking->u.fle2.type) {
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT:
                return _mongocrypt_fle2_placeholder_to_insert_update_ciphertextForTextSearch(kb,
                                                                                             marking,
                                                                                             ciphertext,
                                                                                             status);
            case MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND:
                return _mongocrypt_fle2_placeholder_to_find_ciphertextForTextSearch(kb, marking, ciphertext, status);
            default: CLIENT_ERR("unexpected fle2 type: %d", (int)marking->u.fle2.type); return false;
            }
        default: CLIENT_ERR("unexpected algorithm: %d", (int)marking->u.fle1.algorithm); return false;
        }
    case MONGOCRYPT_MARKING_FLE1_BY_ID:
    case MONGOCRYPT_MARKING_FLE1_BY_ALTNAME:
        return _mongocrypt_fle1_marking_to_ciphertext(kb, marking, ciphertext, status);
    default: CLIENT_ERR("unexpected marking type: %d", (int)marking->type); return false;
    }
}
