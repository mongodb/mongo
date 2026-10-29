// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/crypto/symmetric_key.h"

#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

std::string SymmetricKeyId::_initStrRep() const {
    return str::stream() << _name << " (" << _id << ")";
}

const std::string& SymmetricKeyId::toString() const {
    if (!_strRep.empty()) {
        return _strRep;
    } else {
        return _name;
    }
}

SymmetricKey::SymmetricKey(const uint8_t* key,
                           size_t keySize,
                           uint32_t algorithm,
                           SymmetricKeyId keyId,
                           uint32_t initializationCount)
    : _algorithm(algorithm),
      _keySize(keySize),
      _key(key, key + keySize),
      _keyId(std::move(keyId)),
      _initializationCount(initializationCount),
      _invocationCount(0) {
    if (_keySize < crypto::minKeySize || _keySize > crypto::maxKeySize) {
        LOGV2_ERROR(
            23866, "Attempt to construct symmetric key of invalid size", "size"_attr = _keySize);
        return;
    }
}

SymmetricKey::SymmetricKey(SecureVector<uint8_t> key, uint32_t algorithm, SymmetricKeyId keyId)
    : _algorithm(algorithm),
      _keySize(key->size()),
      _key(std::move(key)),
      _keyId(std::move(keyId)),
      _initializationCount(1),
      _invocationCount(0) {}

SymmetricKey::SymmetricKey(SymmetricKey&& sk)
    : _algorithm(sk._algorithm),
      _keySize(sk._keySize),
      _key(std::move(sk._key)),
      _keyId(std::move(sk._keyId)),
      _initializationCount(sk._initializationCount),
      _invocationCount(sk._invocationCount.load()) {}

SymmetricKey& SymmetricKey::operator=(SymmetricKey&& sk) {
    _algorithm = sk._algorithm;
    _keySize = sk._keySize;
    _key = std::move(sk._key);
    _keyId = std::move(sk._keyId);
    _initializationCount = sk._initializationCount;
    _invocationCount.store(sk._invocationCount.load());

    return *this;
}
}  // namespace mongo
