// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/sha512_block.h"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

namespace mongo {
using CDRinit = std::initializer_list<ConstDataRange>;

void SHA1BlockTraits::computeHash(CDRinit input, HashType* const output) {
    CC_SHA1_CTX ctx;
    CC_SHA1_Init(&ctx);
    for (const auto& range : input) {
        CC_SHA1_Update(&ctx, range.data(), range.length());
    }

    static_assert(sizeof(*output) == CC_SHA1_DIGEST_LENGTH,
                  "SHA1 HashType size doesn't match expected digest output size");
    CC_SHA1_Final(output->data(), &ctx);
}

void SHA256BlockTraits::computeHash(CDRinit input, HashType* const output) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    for (const auto& range : input) {
        CC_SHA256_Update(&ctx, range.data(), range.length());
    }

    static_assert(sizeof(*output) == CC_SHA256_DIGEST_LENGTH,
                  "SHA256 HashType size doesn't match expected digest output size");
    CC_SHA256_Final(output->data(), &ctx);
}

void SHA512BlockTraits::computeHash(CDRinit input, HashType* const output) {
    CC_SHA512_CTX ctx;
    CC_SHA512_Init(&ctx);
    for (const auto& range : input) {
        CC_SHA512_Update(&ctx, range.data(), range.length());
    }

    static_assert(sizeof(*output) == CC_SHA512_DIGEST_LENGTH,
                  "SHA512 HashType size doesn't match expected digest output size");
    CC_SHA512_Final(output->data(), &ctx);
}

void SHA256BlockTraits::computeHashWithCtx(HashContext*,
                                           std::initializer_list<ConstDataRange> input,
                                           HashType* const output) {
    return SHA256BlockTraits::computeHash(input, output);
}

void SHA1BlockTraits::computeHmac(const uint8_t* key,
                                  size_t keyLen,
                                  CDRinit input,
                                  SHA1BlockTraits::HashType* const output) {
    static_assert(sizeof(*output) == CC_SHA1_DIGEST_LENGTH,
                  "SHA1 HashType size doesn't match expected hmac output size");
    CCHmacContext ctx;
    CCHmacInit(&ctx, kCCHmacAlgSHA1, key, keyLen);
    for (const auto& range : input) {
        CCHmacUpdate(&ctx, range.data(), range.length());
    }
    CCHmacFinal(&ctx, output);
}

void SHA1BlockTraits::computeHmacWithCtx(HmacContext*,
                                         const uint8_t* key,
                                         size_t keyLen,
                                         std::initializer_list<ConstDataRange> input,
                                         HashType* const output) {
    return SHA1BlockTraits::computeHmac(key, keyLen, input, output);
}


void SHA256BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    CDRinit input,
                                    SHA256BlockTraits::HashType* const output) {
    static_assert(sizeof(*output) == CC_SHA256_DIGEST_LENGTH,
                  "SHA256 HashType size doesn't match expected hmac output size");
    CCHmacContext ctx;
    CCHmacInit(&ctx, kCCHmacAlgSHA256, key, keyLen);
    for (const auto& range : input) {
        CCHmacUpdate(&ctx, range.data(), range.length());
    }
    CCHmacFinal(&ctx, output);
}

void SHA256BlockTraits::computeHmacWithCtx(HmacContext*,
                                           const uint8_t* key,
                                           size_t keyLen,
                                           std::initializer_list<ConstDataRange> input,
                                           HashType* const output) {
    return SHA256BlockTraits::computeHmac(key, keyLen, input, output);
}


void SHA512BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    CDRinit input,
                                    SHA512BlockTraits::HashType* const output) {
    static_assert(sizeof(*output) == CC_SHA512_DIGEST_LENGTH,
                  "SHA512 HashType size doesn't match expected hmac output size");
    CCHmacContext ctx;
    CCHmacInit(&ctx, kCCHmacAlgSHA512, key, keyLen);
    for (const auto& range : input) {
        CCHmacUpdate(&ctx, range.data(), range.length());
    }
    CCHmacFinal(&ctx, output);
}

void SHA512BlockTraits::computeHmacWithCtx(HmacContext*,
                                           const uint8_t* key,
                                           size_t keyLen,
                                           std::initializer_list<ConstDataRange> input,
                                           HashType* const output) {
    return SHA512BlockTraits::computeHmac(key, keyLen, input, output);
}

}  // namespace mongo
