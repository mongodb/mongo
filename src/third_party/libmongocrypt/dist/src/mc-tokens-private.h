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
#ifndef MONGOCRYPT_TOKENS_PRIVATE_H
#define MONGOCRYPT_TOKENS_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-crypto-private.h"

/*
 * ======================= Begin: FLE 2 Token Reference =======================
 *
 * v is a BSON value. It is the bytes after "e_name" in "element" in
 * https://bsonspec.org/spec.html.
 * u is a "contention factor". It is a uint64_t.
 * HMAC is the HMAC-SHA-256 function.
 * Integers are represented as uint64_t in little-endian.
 *
 * CollectionsLevel1Token = HMAC(RootKey, 1)
 * ServerDataEncryptionLevel1Token = HMAC(RootKey, 3)
 *
 * EDCToken = HMAC(CollectionsLevel1Token, 1)
 * ESCToken = HMAC(CollectionsLevel1Token, 2)
 * ECCToken = HMAC(CollectionsLevel1Token, 3)
 * ECOCToken = HMAC(CollectionsLevel1Token, 4)
 *
 * EDCDerivedFromDataToken = HMAC(EDCToken, v)
 * ESCDerivedFromDataToken = HMAC(ESCToken, v)
 * ECCDerivedFromDataToken = HMAC(ECCToken, v)
 *
 * EDCDerivedFromDataTokenAndContentionFactor = HMAC(EDCDerivedFromDataToken, u)
 * ESCDerivedFromDataTokenAndContentionFactor = HMAC(ESCDerivedFromDataToken, u)
 * ECCDerivedFromDataTokenAndContentionFactor = HMAC(ECCDerivedFromDataToken, u)
 *
 * EDCTwiceDerivedToken      = HMAC(EDCDerivedFromDataTokenAndContentionFactor, 1)

 * ESCTwiceDerivedTagToken   = HMAC(ESCDerivedFromDataTokenAndContentionFactor, 1)
 * ESCTwiceDerivedValueToken = HMAC(ESCDerivedFromDataTokenAndContentionFactor, 2)

 * ECCTwiceDerivedTagToken   = HMAC(ECCDerivedFromDataTokenAndContentionFactor, 1)
 * ECCTwiceDerivedValueToken = HMAC(ECCDerivedFromDataTokenAndContentionFactor, 2)
 *
 * Note: ECC related tokens are used in FLE2v1 only.
 *       Further, ECCTwiceDerivedValue(Tag|Token) have been omitted entirely.
 *       The above comment describing derivation is for doc purposes only.
 * ----------------------------------------------------------------------------
 * Added in FLE2v2:
 *
 * ServerTokenDerivationLevel1Token = HMAC(RootKey, 2)
 * ServerDerivedFromDataToken = HMAC(ServerTokenDerivationLevel1Token, v)
 *
 * ServerCountAndContentionFactorEncryptionToken =
 *    HMAC(ServerDerivedFromDataToken, 1)
 * ServerZerosEncryptionToken = HMAC(ServerDerivedFromDataToken, 2)
 * ----------------------------------------------------------------------------
 * Added in Range V2:
 *
 * d is a 17-byte blob of zeros.
 *
 * AnchorPaddingTokenRoot   = HMAC(ESCToken, d)
 * Server-side:
 *      AnchorPaddingTokenKey    = HMAC(AnchorPaddingTokenRoot, 1)
 *      AnchorPaddingTokenValue  = HMAC(AnchorPaddingTokenRoot, 2)
 * ======================== End: FLE 2 Token Reference ========================
 */

/// Declare a token type named 'Name', with constructor parameters given by the
/// remaining arguments. Each constructor will also have the implicit first
/// argument '_mongocrypt_crypto_t* crypto' and a final argument
/// 'mongocrypt_status_t* status'
#define DECL_TOKEN_TYPE(Name, ...) DECL_TOKEN_TYPE_1(Name, BSON_CONCAT(Name, _t), __VA_ARGS__)

#define DECL_TOKEN_TYPE_1(Prefix, T, ...)                                                                              \
    /* Opaque typedef the struct */                                                                                    \
    typedef struct T T;                                                                                                \
    /* Data-getter */                                                                                                  \
    extern const _mongocrypt_buffer_t *BSON_CONCAT(Prefix, _get)(const T *t);                                          \
    /* Destructor */                                                                                                   \
    extern void BSON_CONCAT(Prefix, _destroy)(T * t);                                                                  \
    /* Constructor for server to shallow copy tokens from raw buffer */                                                \
    extern T *BSON_CONCAT(Prefix, _new_from_buffer)(_mongocrypt_buffer_t * buf);                                       \
    /* Constructor for server to deep copy tokens from raw buffer */                                                   \
    extern T *BSON_CONCAT(Prefix, _new_from_buffer_copy)(_mongocrypt_buffer_t * buf);                                  \
    /* Constructor. Parameter list given as variadic args */                                                           \
    extern T *BSON_CONCAT(Prefix, _new)(_mongocrypt_crypto_t * crypto, __VA_ARGS__, mongocrypt_status_t * status)

DECL_TOKEN_TYPE(mc_CollectionsLevel1Token, const _mongocrypt_buffer_t *);
DECL_TOKEN_TYPE(mc_ServerTokenDerivationLevel1Token, const _mongocrypt_buffer_t *);
DECL_TOKEN_TYPE(mc_ServerDataEncryptionLevel1Token, const _mongocrypt_buffer_t *);

DECL_TOKEN_TYPE(mc_EDCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token);
DECL_TOKEN_TYPE(mc_ESCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token);
DECL_TOKEN_TYPE(mc_ECCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token);
DECL_TOKEN_TYPE(mc_ECOCToken, const mc_CollectionsLevel1Token_t *CollectionsLevel1Token);

DECL_TOKEN_TYPE(mc_EDCDerivedFromDataToken, const mc_EDCToken_t *EDCToken, const _mongocrypt_buffer_t *v);
DECL_TOKEN_TYPE(mc_ECCDerivedFromDataToken, const mc_ECCToken_t *ECCToken, const _mongocrypt_buffer_t *v);
DECL_TOKEN_TYPE(mc_ESCDerivedFromDataToken, const mc_ESCToken_t *ESCToken, const _mongocrypt_buffer_t *v);

DECL_TOKEN_TYPE(mc_EDCDerivedFromDataTokenAndContentionFactor,
                const mc_EDCDerivedFromDataToken_t *EDCDerivedFromDataToken,
                uint64_t u);
DECL_TOKEN_TYPE(mc_ESCDerivedFromDataTokenAndContentionFactor,
                const mc_ESCDerivedFromDataToken_t *ESCDerivedFromDataToken,
                uint64_t u);
DECL_TOKEN_TYPE(mc_ECCDerivedFromDataTokenAndContentionFactor,
                const mc_ECCDerivedFromDataToken_t *ECCDerivedFromDataToken,
                uint64_t u);

DECL_TOKEN_TYPE(mc_EDCTwiceDerivedToken,
                const mc_EDCDerivedFromDataTokenAndContentionFactor_t *EDCDerivedFromDataTokenAndContentionFactor);
DECL_TOKEN_TYPE(mc_ESCTwiceDerivedTagToken,
                const mc_ESCDerivedFromDataTokenAndContentionFactor_t *ESCDerivedFromDataTokenAndContentionFactor);
DECL_TOKEN_TYPE(mc_ESCTwiceDerivedValueToken,
                const mc_ESCDerivedFromDataTokenAndContentionFactor_t *ESCDerivedFromDataTokenAndContentionFactor);

DECL_TOKEN_TYPE(mc_ServerDerivedFromDataToken,
                const mc_ServerTokenDerivationLevel1Token_t *ServerTokenDerivationToken,
                const _mongocrypt_buffer_t *v);

DECL_TOKEN_TYPE(mc_ServerCountAndContentionFactorEncryptionToken,
                const mc_ServerDerivedFromDataToken_t *serverDerivedFromDataToken);
DECL_TOKEN_TYPE(mc_ServerZerosEncryptionToken, const mc_ServerDerivedFromDataToken_t *serverDerivedFromDataToken);

DECL_TOKEN_TYPE(mc_AnchorPaddingTokenRoot, const mc_ESCToken_t *ESCToken);
DECL_TOKEN_TYPE(mc_AnchorPaddingKeyToken, const mc_AnchorPaddingTokenRoot_t *anchorPaddingToken);
DECL_TOKEN_TYPE(mc_AnchorPaddingValueToken, const mc_AnchorPaddingTokenRoot_t *anchorPaddingToken);

#undef DECL_TOKEN_TYPE
#undef DECL_TOKEN_TYPE_1

#endif /* MONGOCRYPT_TOKENS_PRIVATE_H */
