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

#include "mc-tokens-private.h"
#include "mongocrypt-buffer-private.h"

/// Define a token type of the given name, with constructor parameters given as
/// the remaining arguments. This macro usage should be followed by the
/// constructor body, with the implicit first argument '_mongocrypt_crypto_t*
/// crypto' and final argument 'mongocrypt_status_t* status'
#define DEF_TOKEN_TYPE(Name, ...) DEF_TOKEN_TYPE_1(Name, BSON_CONCAT(Name, _t), __VA_ARGS__)

#define DEF_TOKEN_TYPE_1(Prefix, T, ...)                                                                               \
    /* Define the struct for the token */                                                                              \
    struct T {                                                                                                         \
        _mongocrypt_buffer_t data;                                                                                     \
    };                                                                                                                 \
    /* Data-getter */                                                                                                  \
    const _mongocrypt_buffer_t *BSON_CONCAT(Prefix, _get)(const T *self) { return &self->data; }                       \
    /* Destructor */                                                                                                   \
    void BSON_CONCAT(Prefix, _destroy)(T * self) {                                                                     \
        if (!self) {                                                                                                   \
            return;                                                                                                    \
        }                                                                                                              \
        _mongocrypt_buffer_cleanup(&self->data);                                                                       \
        bson_free(self);                                                                                               \
    }                                                                                                                  \
    /* Constructor. Shallow copy from raw buffer */                                                                    \
    T *BSON_CONCAT(Prefix, _new_from_buffer)(const _mongocrypt_buffer_t *buf) {                                        \
        BSON_ASSERT(buf->len == MONGOCRYPT_HMAC_SHA256_LEN);                                                           \
        T *t = bson_malloc(sizeof(T));                                                                                 \
        _mongocrypt_buffer_set_to(buf, &t->data);                                                                      \
        return t;                                                                                                      \
    }                                                                                                                  \
    /* Constructor. Deep copy from raw buffer */                                                                       \
    T *BSON_CONCAT(Prefix, _new_from_buffer_copy)(const _mongocrypt_buffer_t *buf) {                                   \
        BSON_ASSERT(buf->len == MONGOCRYPT_HMAC_SHA256_LEN);                                                           \
        T *t = bson_malloc(sizeof(T));                                                                                 \
        _mongocrypt_buffer_init(&t->data);                                                                             \
        _mongocrypt_buffer_copy_to(buf, &t->data);                                                                     \
        return t;                                                                                                      \
    }                                                                                                                  \
    /* Constructor. Parameter list given as variadic args. */                                                          \
    T *BSON_CONCAT(Prefix, _new)(_mongocrypt_crypto_t * crypto, __VA_ARGS__, mongocrypt_status_t * status)

#define IMPL_TOKEN_NEW_1(Name, Key, Arg, Clean)                                                                        \
    {                                                                                                                  \
        BSON_CONCAT(Name, _t) *t = bson_malloc(sizeof(BSON_CONCAT(Name, _t)));                                         \
        _mongocrypt_buffer_init(&t->data);                                                                             \
        _mongocrypt_buffer_resize(&t->data, MONGOCRYPT_HMAC_SHA256_LEN);                                               \
                                                                                                                       \
        if (!_mongocrypt_hmac_sha_256(crypto, Key, Arg, &t->data, status)) {                                           \
            BSON_CONCAT(Name, _destroy)(t);                                                                            \
            Clean;                                                                                                     \
            return NULL;                                                                                               \
        }                                                                                                              \
        Clean;                                                                                                         \
        return t;                                                                                                      \
    }

// Define the implementation of a token where Arg is a _mongocrypt_buffer_t.
#define IMPL_TOKEN_NEW(Name, Key, Arg) IMPL_TOKEN_NEW_1(Name, Key, Arg, (void)0)

// Define the implementation of a token where Arg is a uint64_t.
#define IMPL_TOKEN_NEW_CONST(Name, Key, Arg)                                                                           \
    {                                                                                                                  \
        _mongocrypt_buffer_t to_hash;                                                                                  \
        _mongocrypt_buffer_copy_from_uint64_le(&to_hash, Arg);                                                         \
        IMPL_TOKEN_NEW_1(Name, Key, &to_hash, _mongocrypt_buffer_cleanup(&to_hash))                                    \
    }

DEF_TOKEN_TYPE(mc_CollectionsLevel1Token, const _mongocrypt_buffer_t *RootKey)
IMPL_TOKEN_NEW_CONST(mc_CollectionsLevel1Token, RootKey, 1)

DEF_TOKEN_TYPE(mc_EDCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_EDCToken, mc_CollectionsLevel1Token_get(CollectionsLevel1Token), 1)

DEF_TOKEN_TYPE(mc_ESCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_ESCToken, mc_CollectionsLevel1Token_get(CollectionsLevel1Token), 2)

DEF_TOKEN_TYPE(mc_ECCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_ECCToken, mc_CollectionsLevel1Token_get(CollectionsLevel1Token), 3)

DEF_TOKEN_TYPE(mc_ECOCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_ECOCToken, mc_CollectionsLevel1Token_get(CollectionsLevel1Token), 4)

DEF_TOKEN_TYPE(mc_EDCDerivedFromDataToken, const mc_EDCToken_t *EDCToken, const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_EDCDerivedFromDataToken, mc_EDCToken_get(EDCToken), v)

DEF_TOKEN_TYPE(mc_ESCDerivedFromDataToken, const mc_ESCToken_t *ESCToken, const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_ESCDerivedFromDataToken, mc_ESCToken_get(ESCToken), v)

DEF_TOKEN_TYPE(mc_ECCDerivedFromDataToken, const mc_ECCToken_t *ECCToken, const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_ECCDerivedFromDataToken, mc_ECCToken_get(ECCToken), v)

DEF_TOKEN_TYPE(mc_EDCTwiceDerivedToken,
               const mc_EDCDerivedFromDataTokenAndContentionFactor_t *EDCDerivedFromDataTokenAndContentionFactor)
IMPL_TOKEN_NEW_CONST(mc_EDCTwiceDerivedToken,
                     mc_EDCDerivedFromDataTokenAndContentionFactor_get(EDCDerivedFromDataTokenAndContentionFactor),
                     1)

DEF_TOKEN_TYPE(mc_ESCTwiceDerivedTagToken,
               const mc_ESCDerivedFromDataTokenAndContentionFactor_t *ESCDerivedFromDataTokenAndContentionFactor)
IMPL_TOKEN_NEW_CONST(mc_ESCTwiceDerivedTagToken,
                     mc_ESCDerivedFromDataTokenAndContentionFactor_get(ESCDerivedFromDataTokenAndContentionFactor),
                     1)
DEF_TOKEN_TYPE(mc_ESCTwiceDerivedValueToken,
               const mc_ESCDerivedFromDataTokenAndContentionFactor_t *ESCDerivedFromDataTokenAndContentionFactor)
IMPL_TOKEN_NEW_CONST(mc_ESCTwiceDerivedValueToken,
                     mc_ESCDerivedFromDataTokenAndContentionFactor_get(ESCDerivedFromDataTokenAndContentionFactor),
                     2)

DEF_TOKEN_TYPE(mc_ServerDataEncryptionLevel1Token, const _mongocrypt_buffer_t *RootKey)
IMPL_TOKEN_NEW_CONST(mc_ServerDataEncryptionLevel1Token, RootKey, 3)

DEF_TOKEN_TYPE(mc_EDCDerivedFromDataTokenAndContentionFactor,
               const mc_EDCDerivedFromDataToken_t *EDCDerivedFromDataToken,
               uint64_t u)
IMPL_TOKEN_NEW_CONST(mc_EDCDerivedFromDataTokenAndContentionFactor,
                     mc_EDCDerivedFromDataToken_get(EDCDerivedFromDataToken),
                     u)

DEF_TOKEN_TYPE(mc_ESCDerivedFromDataTokenAndContentionFactor,
               const mc_ESCDerivedFromDataToken_t *ESCDerivedFromDataToken,
               uint64_t u)
IMPL_TOKEN_NEW_CONST(mc_ESCDerivedFromDataTokenAndContentionFactor,
                     mc_ESCDerivedFromDataToken_get(ESCDerivedFromDataToken),
                     u)

DEF_TOKEN_TYPE(mc_ECCDerivedFromDataTokenAndContentionFactor,
               const mc_ECCDerivedFromDataToken_t *ECCDerivedFromDataToken,
               uint64_t u)
IMPL_TOKEN_NEW_CONST(mc_ECCDerivedFromDataTokenAndContentionFactor,
                     mc_ECCDerivedFromDataToken_get(ECCDerivedFromDataToken),
                     u)

/* FLE2v2 */

DEF_TOKEN_TYPE(mc_ServerTokenDerivationLevel1Token, const _mongocrypt_buffer_t *RootKey)
IMPL_TOKEN_NEW_CONST(mc_ServerTokenDerivationLevel1Token, RootKey, 2)

DEF_TOKEN_TYPE(mc_ServerDerivedFromDataToken,
               const mc_ServerTokenDerivationLevel1Token_t *ServerTokenDerivationToken,
               const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_ServerDerivedFromDataToken, mc_ServerTokenDerivationLevel1Token_get(ServerTokenDerivationToken), v)

DEF_TOKEN_TYPE(mc_ServerCountAndContentionFactorEncryptionToken,
               const mc_ServerDerivedFromDataToken_t *serverDerivedFromDataToken)
IMPL_TOKEN_NEW_CONST(mc_ServerCountAndContentionFactorEncryptionToken,
                     mc_ServerDerivedFromDataToken_get(serverDerivedFromDataToken),
                     1)

DEF_TOKEN_TYPE(mc_ServerZerosEncryptionToken, const mc_ServerDerivedFromDataToken_t *serverDerivedFromDataToken)
IMPL_TOKEN_NEW_CONST(mc_ServerZerosEncryptionToken, mc_ServerDerivedFromDataToken_get(serverDerivedFromDataToken), 2)

// d = 17 bytes of 0, AnchorPaddingTokenRoot = HMAC(ESCToken, d)
#define ANCHOR_PADDING_TOKEN_D_LENGTH 17
const uint8_t mc_AnchorPaddingTokenDValue[ANCHOR_PADDING_TOKEN_D_LENGTH] =
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

DEF_TOKEN_TYPE(mc_AnchorPaddingTokenRoot, const mc_ESCToken_t *ESCToken) {
    _mongocrypt_buffer_t to_hash;
    if (!_mongocrypt_buffer_copy_from_data_and_size(&to_hash,
                                                    mc_AnchorPaddingTokenDValue,
                                                    ANCHOR_PADDING_TOKEN_D_LENGTH)) {
        return NULL;
    }
    IMPL_TOKEN_NEW_1(mc_AnchorPaddingTokenRoot,
                     mc_ESCToken_get(ESCToken),
                     &to_hash,
                     _mongocrypt_buffer_cleanup(&to_hash))
}

#undef ANCHOR_PADDING_TOKEN_D_LENGTH

DEF_TOKEN_TYPE(mc_AnchorPaddingKeyToken, const mc_AnchorPaddingTokenRoot_t *anchorPaddingToken)
IMPL_TOKEN_NEW_CONST(mc_AnchorPaddingKeyToken, mc_AnchorPaddingTokenRoot_get(anchorPaddingToken), 1)

DEF_TOKEN_TYPE(mc_AnchorPaddingValueToken, const mc_AnchorPaddingTokenRoot_t *anchorPaddingToken)
IMPL_TOKEN_NEW_CONST(mc_AnchorPaddingValueToken, mc_AnchorPaddingTokenRoot_get(anchorPaddingToken), 2)

#define TEXT_EXACT_ID 1
#define TEXT_SUBSTRING_ID 2
#define TEXT_SUFFIX_ID 3
#define TEXT_PREFIX_ID 4

DEF_TOKEN_TYPE(mc_EDCTextExactToken, const mc_EDCToken_t *edcToken)
IMPL_TOKEN_NEW_CONST(mc_EDCTextExactToken, mc_EDCToken_get(edcToken), TEXT_EXACT_ID)
DEF_TOKEN_TYPE(mc_EDCTextSubstringToken, const mc_EDCToken_t *edcToken)
IMPL_TOKEN_NEW_CONST(mc_EDCTextSubstringToken, mc_EDCToken_get(edcToken), TEXT_SUBSTRING_ID)
DEF_TOKEN_TYPE(mc_EDCTextSuffixToken, const mc_EDCToken_t *edcToken)
IMPL_TOKEN_NEW_CONST(mc_EDCTextSuffixToken, mc_EDCToken_get(edcToken), TEXT_SUFFIX_ID)
DEF_TOKEN_TYPE(mc_EDCTextPrefixToken, const mc_EDCToken_t *edcToken)
IMPL_TOKEN_NEW_CONST(mc_EDCTextPrefixToken, mc_EDCToken_get(edcToken), TEXT_PREFIX_ID)

DEF_TOKEN_TYPE(mc_ESCTextExactToken, const mc_ESCToken_t *escToken)
IMPL_TOKEN_NEW_CONST(mc_ESCTextExactToken, mc_ESCToken_get(escToken), TEXT_EXACT_ID)
DEF_TOKEN_TYPE(mc_ESCTextSubstringToken, const mc_ESCToken_t *escToken)
IMPL_TOKEN_NEW_CONST(mc_ESCTextSubstringToken, mc_ESCToken_get(escToken), TEXT_SUBSTRING_ID)
DEF_TOKEN_TYPE(mc_ESCTextSuffixToken, const mc_ESCToken_t *escToken)
IMPL_TOKEN_NEW_CONST(mc_ESCTextSuffixToken, mc_ESCToken_get(escToken), TEXT_SUFFIX_ID)
DEF_TOKEN_TYPE(mc_ESCTextPrefixToken, const mc_ESCToken_t *escToken)
IMPL_TOKEN_NEW_CONST(mc_ESCTextPrefixToken, mc_ESCToken_get(escToken), TEXT_PREFIX_ID)

DEF_TOKEN_TYPE(mc_ServerTextExactToken, const mc_ServerTokenDerivationLevel1Token_t *serverTokenDerivationLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_ServerTextExactToken,
                     mc_ServerTokenDerivationLevel1Token_get(serverTokenDerivationLevel1Token),
                     TEXT_EXACT_ID)
DEF_TOKEN_TYPE(mc_ServerTextSubstringToken,
               const mc_ServerTokenDerivationLevel1Token_t *serverTokenDerivationLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_ServerTextSubstringToken,
                     mc_ServerTokenDerivationLevel1Token_get(serverTokenDerivationLevel1Token),
                     TEXT_SUBSTRING_ID)
DEF_TOKEN_TYPE(mc_ServerTextSuffixToken, const mc_ServerTokenDerivationLevel1Token_t *serverTokenDerivationLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_ServerTextSuffixToken,
                     mc_ServerTokenDerivationLevel1Token_get(serverTokenDerivationLevel1Token),
                     TEXT_SUFFIX_ID)
DEF_TOKEN_TYPE(mc_ServerTextPrefixToken, const mc_ServerTokenDerivationLevel1Token_t *serverTokenDerivationLevel1Token)
IMPL_TOKEN_NEW_CONST(mc_ServerTextPrefixToken,
                     mc_ServerTokenDerivationLevel1Token_get(serverTokenDerivationLevel1Token),
                     TEXT_PREFIX_ID)

#define IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(Name, Key, BufferArg, UintArg)                                         \
    {                                                                                                                  \
        BSON_CONCAT(Name, _t) *t = bson_malloc(sizeof(BSON_CONCAT(Name, _t)));                                         \
        _mongocrypt_buffer_t tmp;                                                                                      \
        _mongocrypt_buffer_init(&tmp);                                                                                 \
        _mongocrypt_buffer_resize(&tmp, MONGOCRYPT_HMAC_SHA256_LEN);                                                   \
        _mongocrypt_buffer_init(&t->data);                                                                             \
        _mongocrypt_buffer_resize(&t->data, MONGOCRYPT_HMAC_SHA256_LEN);                                               \
        if (!_mongocrypt_hmac_sha_256(crypto, Key, BufferArg, &tmp, status)) {                                         \
            BSON_CONCAT(Name, _destroy)(t);                                                                            \
            _mongocrypt_buffer_cleanup(&tmp);                                                                          \
            return NULL;                                                                                               \
        }                                                                                                              \
        _mongocrypt_buffer_t uint_arg;                                                                                 \
        _mongocrypt_buffer_copy_from_uint64_le(&uint_arg, UintArg);                                                    \
        if (!_mongocrypt_hmac_sha_256(crypto, &tmp, &uint_arg, &t->data, status)) {                                    \
            BSON_CONCAT(Name, _destroy)(t);                                                                            \
            _mongocrypt_buffer_cleanup(&tmp);                                                                          \
            _mongocrypt_buffer_cleanup(&uint_arg);                                                                     \
            return NULL;                                                                                               \
        }                                                                                                              \
        _mongocrypt_buffer_cleanup(&tmp);                                                                              \
        _mongocrypt_buffer_cleanup(&uint_arg);                                                                         \
        return t;                                                                                                      \
    }

DEF_TOKEN_TYPE(mc_EDCTextExactDerivedFromDataTokenAndContentionFactorToken,
               const mc_EDCTextExactToken_t *edcTextExactToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_EDCTextExactDerivedFromDataTokenAndContentionFactorToken,
                                        mc_EDCTextExactToken_get(edcTextExactToken),
                                        v,
                                        u)
DEF_TOKEN_TYPE(mc_EDCTextSubstringDerivedFromDataTokenAndContentionFactorToken,
               const mc_EDCTextSubstringToken_t *edcTextSubstringToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_EDCTextSubstringDerivedFromDataTokenAndContentionFactorToken,
                                        mc_EDCTextSubstringToken_get(edcTextSubstringToken),
                                        v,
                                        u)
DEF_TOKEN_TYPE(mc_EDCTextSuffixDerivedFromDataTokenAndContentionFactorToken,
               const mc_EDCTextSuffixToken_t *edcTextSuffixToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_EDCTextSuffixDerivedFromDataTokenAndContentionFactorToken,
                                        mc_EDCTextSuffixToken_get(edcTextSuffixToken),
                                        v,
                                        u)
DEF_TOKEN_TYPE(mc_EDCTextPrefixDerivedFromDataTokenAndContentionFactorToken,
               const mc_EDCTextPrefixToken_t *edcTextPrefixToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_EDCTextPrefixDerivedFromDataTokenAndContentionFactorToken,
                                        mc_EDCTextPrefixToken_get(edcTextPrefixToken),
                                        v,
                                        u)

DEF_TOKEN_TYPE(mc_ESCTextExactDerivedFromDataTokenAndContentionFactorToken,
               const mc_ESCTextExactToken_t *escTextExactToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_ESCTextExactDerivedFromDataTokenAndContentionFactorToken,
                                        mc_ESCTextExactToken_get(escTextExactToken),
                                        v,
                                        u)
DEF_TOKEN_TYPE(mc_ESCTextSubstringDerivedFromDataTokenAndContentionFactorToken,
               const mc_ESCTextSubstringToken_t *escTextSubstringToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_ESCTextSubstringDerivedFromDataTokenAndContentionFactorToken,
                                        mc_ESCTextSubstringToken_get(escTextSubstringToken),
                                        v,
                                        u)
DEF_TOKEN_TYPE(mc_ESCTextSuffixDerivedFromDataTokenAndContentionFactorToken,
               const mc_ESCTextSuffixToken_t *escTextSuffixToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_ESCTextSuffixDerivedFromDataTokenAndContentionFactorToken,
                                        mc_ESCTextSuffixToken_get(escTextSuffixToken),
                                        v,
                                        u)
DEF_TOKEN_TYPE(mc_ESCTextPrefixDerivedFromDataTokenAndContentionFactorToken,
               const mc_ESCTextPrefixToken_t *escTextPrefixToken,
               const _mongocrypt_buffer_t *v,
               uint64_t u)
IMPL_TOKEN_NEW_FROM_DATA_AND_CONTENTION(mc_ESCTextPrefixDerivedFromDataTokenAndContentionFactorToken,
                                        mc_ESCTextPrefixToken_get(escTextPrefixToken),
                                        v,
                                        u)

DEF_TOKEN_TYPE(mc_ServerTextExactDerivedFromDataToken,
               const mc_ServerTextExactToken_t *serverTextExactToken,
               const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_ServerTextExactDerivedFromDataToken, mc_ServerTextExactToken_get(serverTextExactToken), v)
DEF_TOKEN_TYPE(mc_ServerTextSubstringDerivedFromDataToken,
               const mc_ServerTextSubstringToken_t *serverTextSubstringToken,
               const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_ServerTextSubstringDerivedFromDataToken, mc_ServerTextSubstringToken_get(serverTextSubstringToken), v)
DEF_TOKEN_TYPE(mc_ServerTextSuffixDerivedFromDataToken,
               const mc_ServerTextSuffixToken_t *serverTextSuffixToken,
               const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_ServerTextSuffixDerivedFromDataToken, mc_ServerTextSuffixToken_get(serverTextSuffixToken), v)
DEF_TOKEN_TYPE(mc_ServerTextPrefixDerivedFromDataToken,
               const mc_ServerTextPrefixToken_t *serverTextPrefixToken,
               const _mongocrypt_buffer_t *v)
IMPL_TOKEN_NEW(mc_ServerTextPrefixDerivedFromDataToken, mc_ServerTextPrefixToken_get(serverTextPrefixToken), v)
