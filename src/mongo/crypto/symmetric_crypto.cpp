/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/crypto/symmetric_crypto.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"

#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace crypto {

MONGO_INITIALIZER(CryptographyInitialized)(InitializerContext* context) {}

size_t aesGetIVSize(crypto::aesMode mode) {
    switch (mode) {
        case crypto::aesMode::cbc:
            return crypto::aesCBCIVSize;
        case crypto::aesMode::gcm:
            return crypto::aesGCMIVSize;
        case crypto::aesMode::ctr:
            return crypto::aesCTRIVSize;
        default:
            fassertFailed(4053);
    }
}

aesMode getCipherModeFromString(const std::string& mode) {
    if (mode == aes256CBCName) {
        return aesMode::cbc;
    } else if (mode == aes256GCMName) {
        return aesMode::gcm;
    } else if (mode == aes256CTRName) {
        return aesMode::ctr;
    } else {
        MONGO_UNREACHABLE;
    }
}

std::string getStringFromCipherMode(aesMode mode) {
    if (mode == aesMode::cbc) {
        return aes256CBCName;
    } else if (mode == aesMode::gcm) {
        return aes256GCMName;
    } else if (mode == aesMode::ctr) {
        return aes256CTRName;
    } else {
        MONGO_UNREACHABLE;
    }
}

SymmetricKey aesGenerate(size_t keySize, SymmetricKeyId keyId) {
    invariant(keySize == sym256KeySize);
    SecureVector<uint8_t> key(keySize);
    SecureRandom().fill(key->data(), key->size());
    return SymmetricKey(std::move(key), aesAlgorithm, std::move(keyId));
}

}  // namespace crypto
}  // namespace mongo
