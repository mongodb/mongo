/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/crypto/fle_tokens.h"

// TODO: Remove after SERVER-95889
#include "mongo/base/init.h"

#include "mongo/crypto/mongocryptbuffer.h"
#include "mongo/crypto/mongocryptstatus.h"

extern "C" {
#include <mc-tokens-private.h>
#include <mongocrypt-crypto-private.h>
}

// A dummy mongocrypt_crypto_t object has to be created in order to call mc_##TokenType##_new from
// libmongocrypt. This can be removed once a mongocrypt_t object exists on ServiceContext (see
// SERVER-95889).
typedef _mongocrypt_crypto_t mongocrypt_crypto_t;

namespace mongo {

auto kEmptyPrfBlock = MongoCryptBuffer::copy(ConstDataRange(PrfBlock{}));

#define FLE_TOKEN_TYPE_MC(TokenType) mc_##TokenType##_t

// Variadic arguments represent parameters for the token's ::deriveFrom function.
#define FLE_TOKEN_CLASS_IMPL_BEGIN(TokenType, ...)                                                \
    void TokenType::initializeFromBuffer(MongoCryptBuffer buffer) {                               \
        _token = mc_##TokenType##_new_from_buffer_copy(buffer.get());                             \
    }                                                                                             \
                                                                                                  \
    TokenType::TokenType() : TokenType(PrfBlock{}) {}                                             \
                                                                                                  \
    TokenType::TokenType(FLE_TOKEN_TYPE_MC(TokenType) * token) {                                  \
        if (token) {                                                                              \
            initializeFromBuffer(MongoCryptBuffer::borrow(mc_##TokenType##_get(token)));          \
        } else {                                                                                  \
            /* When a nullptr token is passed through, create an empty token of the correct size. \
             * This prevents failed asserts in libmongocrypt. */                                  \
            _token = mc_##TokenType##_new_from_buffer(kEmptyPrfBlock.get());                      \
        }                                                                                         \
    }                                                                                             \
                                                                                                  \
    /* Reroutes to call the mc_##TokenType##_t constructor above. */                              \
    TokenType::TokenType(const TokenType& other) : TokenType(other._token) {}                     \
                                                                                                  \
    TokenType& TokenType::operator=(const TokenType& other) {                                     \
        if (this != &other) {                                                                     \
            TokenType tmp(other);                                                                 \
            std::swap(_token, tmp._token);                                                        \
        }                                                                                         \
        return *this;                                                                             \
    }                                                                                             \
                                                                                                  \
    TokenType::TokenType(TokenType&& other) : _token(std::exchange(other._token, nullptr)) {}     \
                                                                                                  \
    TokenType& TokenType::operator=(TokenType&& other) {                                          \
        if (this != &other) {                                                                     \
            mc_##TokenType##_destroy(_token);                                                     \
            _token = std::exchange(other._token, nullptr);                                        \
        }                                                                                         \
        return *this;                                                                             \
    }                                                                                             \
                                                                                                  \
    TokenType::~TokenType() {                                                                     \
        mc_##TokenType##_destroy(_token);                                                         \
    }                                                                                             \
                                                                                                  \
    const FLE_TOKEN_TYPE_MC(TokenType) * TokenType::get() const {                                 \
        return _token;                                                                            \
    }                                                                                             \
                                                                                                  \
    TokenType::TokenType(const PrfBlock& block) {                                                 \
        initializeFromBuffer(MongoCryptBuffer::borrow(ConstDataRange(block)));                    \
    }                                                                                             \
                                                                                                  \
    PrfBlock TokenType::asPrfBlock() const {                                                      \
        PrfBlock block;                                                                           \
        auto buffer = asMongoCryptBuffer();                                                       \
        std::copy(buffer.data(), buffer.data() + buffer.size(), block.data());                    \
        return block;                                                                             \
    }                                                                                             \
                                                                                                  \
    MongoCryptBuffer TokenType::asMongoCryptBuffer() const {                                      \
        return MongoCryptBuffer::borrow(mc_##TokenType##_get(_token));                            \
    }                                                                                             \
                                                                                                  \
    ConstDataRange TokenType::toCDR() const {                                                     \
        return asMongoCryptBuffer().toCDR();                                                      \
    }                                                                                             \
                                                                                                  \
    bool TokenType::operator==(const TokenType& other) const {                                    \
        return (this->name() == other.name() &&                                                   \
                _mongocrypt_buffer_cmp(this->asMongoCryptBuffer().get(),                          \
                                       other.asMongoCryptBuffer().get()) == 0);                   \
    }                                                                                             \
                                                                                                  \
    bool TokenType::operator!=(const TokenType& other) const {                                    \
        return !(*this == other);                                                                 \
    }                                                                                             \
                                                                                                  \
    TokenType TokenType::parse(ConstDataRange block) {                                            \
        uassert(9616300,                                                                          \
                fmt::format("Invalid prf length for {}", #TokenType),                             \
                block.length() == sizeof(PrfBlock));                                              \
                                                                                                  \
        PrfBlock ret;                                                                             \
        std::copy(block.data(), block.data() + block.length(), ret.data());                       \
        return TokenType(std::move(ret));                                                         \
    }                                                                                             \
                                                                                                  \
    TokenType TokenType::deriveFrom(__VA_ARGS__) {                                                \
        /* The beginning of ::deriveFrom is the same for each token. */                           \
        MongoCryptStatus status;                                                                  \
        mongocrypt_crypto_t crypto = {0};                                                         \
        FLE_TOKEN_TYPE_MC(TokenType) * token;


#define FLE_TOKEN_CLASS_IMPL_END(TokenType)                                                  \
    /* The end of ::deriveFrom is the same for each token. Uses memory created in the shared \
     * beginning of ::deriveFrom above. */                                                   \
    uassertStatusOK(status.toStatus());                                                      \
    return TokenType(token);                                                                 \
    }

// Token implementations should all take the form of:
// ```
// FLE_TOKEN_CLASS_IMPL_BEGIN
// {token specific implementation of ::deriveFrom}
// FLE_TOKEN_CLASS_IMPL_END
// ```
// This is to ensure that the open ended ::deriveFrom function gets properly cleaned up and closed.
#define FLE_TOKEN_IMPL_FROM_BUFFER(TokenType)                          \
    FLE_TOKEN_CLASS_IMPL_BEGIN(TokenType, const MongoCryptBuffer& buf) \
    token = mc_##TokenType##_new(&crypto, buf.get(), status);          \
    FLE_TOKEN_CLASS_IMPL_END(TokenType)

#define FLE_TOKEN_IMPL_FROM_TOKEN(TokenType, ParentToken)            \
    FLE_TOKEN_CLASS_IMPL_BEGIN(TokenType, const ParentToken& parent) \
    token = mc_##TokenType##_new(&crypto, parent.get(), status);     \
    FLE_TOKEN_CLASS_IMPL_END(TokenType)

#define FLE_TOKEN_IMPL_FROM_TOKEN_AND_CDR(TokenType, ParentToken)                                \
    FLE_TOKEN_CLASS_IMPL_BEGIN(TokenType, const ParentToken& parent, ConstDataRange cdr)         \
    /* mc_##TokenType##_new for these tokens takes in a _mongocrypt_buffer_t, requiring an extra \
     * conversion step. */                                                                       \
    MongoCryptBuffer argMCB = MongoCryptBuffer::borrow(cdr);                                     \
    token = mc_##TokenType##_new(&crypto, parent.get(), argMCB.get(), status);                   \
    FLE_TOKEN_CLASS_IMPL_END(TokenType)

#define FLE_TOKEN_IMPL_FROM_TOKEN_AND_INT(TokenType, ParentToken)                       \
    FLE_TOKEN_CLASS_IMPL_BEGIN(TokenType, const ParentToken& parent, std::uint64_t arg) \
    token = mc_##TokenType##_new(&crypto, parent.get(), arg, status);                   \
    FLE_TOKEN_CLASS_IMPL_END(TokenType)

// See comments in fle_tokens.h for token derivation details.
FLE_TOKEN_IMPL_FROM_BUFFER(CollectionsLevel1Token)
FLE_TOKEN_IMPL_FROM_BUFFER(ServerDataEncryptionLevel1Token)

FLE_TOKEN_IMPL_FROM_TOKEN(EDCToken, CollectionsLevel1Token)
FLE_TOKEN_IMPL_FROM_TOKEN(ESCToken, CollectionsLevel1Token)
FLE_TOKEN_IMPL_FROM_TOKEN(ECOCToken, CollectionsLevel1Token)

FLE_TOKEN_IMPL_FROM_TOKEN_AND_CDR(EDCDerivedFromDataToken, EDCToken)
FLE_TOKEN_IMPL_FROM_TOKEN_AND_CDR(ESCDerivedFromDataToken, ESCToken)

FLE_TOKEN_IMPL_FROM_TOKEN_AND_INT(EDCDerivedFromDataTokenAndContentionFactor,
                                  EDCDerivedFromDataToken)
FLE_TOKEN_IMPL_FROM_TOKEN_AND_INT(ESCDerivedFromDataTokenAndContentionFactor,
                                  ESCDerivedFromDataToken)

FLE_TOKEN_IMPL_FROM_TOKEN(EDCTwiceDerivedToken, EDCDerivedFromDataTokenAndContentionFactor)
FLE_TOKEN_IMPL_FROM_TOKEN(ESCTwiceDerivedTagToken, ESCDerivedFromDataTokenAndContentionFactor)
FLE_TOKEN_IMPL_FROM_TOKEN(ESCTwiceDerivedValueToken, ESCDerivedFromDataTokenAndContentionFactor)

FLE_TOKEN_IMPL_FROM_BUFFER(ServerTokenDerivationLevel1Token)
FLE_TOKEN_IMPL_FROM_TOKEN_AND_CDR(ServerDerivedFromDataToken, ServerTokenDerivationLevel1Token)
FLE_TOKEN_IMPL_FROM_TOKEN(ServerCountAndContentionFactorEncryptionToken, ServerDerivedFromDataToken)
FLE_TOKEN_IMPL_FROM_TOKEN(ServerZerosEncryptionToken, ServerDerivedFromDataToken)

FLE_TOKEN_IMPL_FROM_TOKEN(AnchorPaddingTokenRoot, ESCToken)
FLE_TOKEN_IMPL_FROM_TOKEN(AnchorPaddingKeyToken, AnchorPaddingTokenRoot)
FLE_TOKEN_IMPL_FROM_TOKEN(AnchorPaddingValueToken, AnchorPaddingTokenRoot)

#undef FLE_TOKEN_TYPE_MC
#undef FLE_TOKEN_CLASS_IMPL_BEGIN
#undef FLE_TOKEN_CLASS_IMPL_END
#undef FLE_TOKEN_IMPL_FROM_BUFFER
#undef FLE_TOKEN_IMPL_FROM_TOKEN
#undef FLE_TOKEN_IMPL_FROM_TOKEN_AND_BUFFER
#undef FLE_TOKEN_IMPL_FROM_TOKEN_AND_BUFFER

// Initialize and immediately destroy a mongocrypt object to ensure that _native_crypto_init gets
// called once in libmongocrypt. This can be removed once a mongocrypt_t object gets attached to
// ServiceContext in SERVER-95889.
MONGO_INITIALIZER(createDestroyMongocrypt_t)(InitializerContext* initializer) {
    mongocrypt_t* tmp_mongocrypt = mongocrypt_new();
    mongocrypt_destroy(tmp_mongocrypt);
}

}  // namespace mongo
