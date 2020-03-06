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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/crypto/symmetric_key.h"

#include <cstring>

#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/logv2/log.h"
#include "mongo/util/secure_zero_memory.h"
#include "mongo/util/str.h"

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
