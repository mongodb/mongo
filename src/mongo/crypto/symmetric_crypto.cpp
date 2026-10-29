// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
