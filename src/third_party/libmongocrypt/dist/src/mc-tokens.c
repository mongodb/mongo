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
    /* Constructor. From raw buffer */                                                                                 \
    T *BSON_CONCAT(Prefix, _new_from_buffer)(_mongocrypt_buffer_t * buf) {                                             \
        BSON_ASSERT(buf->len == MONGOCRYPT_HMAC_SHA256_LEN);                                                           \
        T *t = bson_malloc(sizeof(T));                                                                                 \
        _mongocrypt_buffer_set_to(buf, &t->data);                                                                      \
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
