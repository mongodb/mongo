/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"

namespace mongo {
using CDRinit = std::initializer_list<ConstDataRange>;

SHA1BlockTraits::HashType SHA1BlockTraits::computeHash(CDRinit input) {
    CC_SHA1_CTX ctx;
    CC_SHA1_Init(&ctx);
    for (const auto& range : input) {
        CC_SHA1_Update(&ctx, range.data(), range.length());
    }

    SHA1BlockTraits::HashType ret;
    static_assert(sizeof(ret) == CC_SHA1_DIGEST_LENGTH,
                  "SHA1 HashType size doesn't match expected digest output size");
    CC_SHA1_Final(ret.data(), &ctx);
    return ret;
}

SHA256BlockTraits::HashType SHA256BlockTraits::computeHash(CDRinit input) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    for (const auto& range : input) {
        CC_SHA256_Update(&ctx, range.data(), range.length());
    }

    SHA256BlockTraits::HashType ret;
    static_assert(sizeof(ret) == CC_SHA256_DIGEST_LENGTH,
                  "SHA256 HashType size doesn't match expected digest output size");
    CC_SHA256_Final(ret.data(), &ctx);
    return ret;
}

void SHA1BlockTraits::computeHmac(const uint8_t* key,
                                  size_t keyLen,
                                  const uint8_t* input,
                                  size_t inputLen,
                                  SHA1BlockTraits::HashType* const output) {
    static_assert(sizeof(*output) == CC_SHA1_DIGEST_LENGTH,
                  "SHA1 HashType size doesn't match expected hmac output size");
    CCHmacContext ctx;
    CCHmacInit(&ctx, kCCHmacAlgSHA1, key, keyLen);
    CCHmacUpdate(&ctx, input, inputLen);
    CCHmacFinal(&ctx, output);
}

void SHA256BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    const uint8_t* input,
                                    size_t inputLen,
                                    SHA256BlockTraits::HashType* const output) {
    static_assert(sizeof(*output) == CC_SHA256_DIGEST_LENGTH,
                  "SHA256 HashType size doesn't match expected hmac output size");
    CCHmacContext ctx;
    CCHmacInit(&ctx, kCCHmacAlgSHA256, key, keyLen);
    CCHmacUpdate(&ctx, input, inputLen);
    CCHmacFinal(&ctx, output);
}

}  // namespace mongo
