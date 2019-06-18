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

#pragma once

#include <cstdint>
#include <memory>

#include "mongo/base/secure_allocator.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {
class Status;

class SymmetricKeyId {
public:
    using id_type = std::uint64_t;

    template <typename StringLike>
    SymmetricKeyId(const StringLike& name, id_type id)
        : _id(id), _name(name), _strRep(_initStrRep()) {}

    template <typename StringLike>
    SymmetricKeyId(const StringLike& name) : _name(name) {}

    const std::string& toString() const;

    bool operator==(const SymmetricKeyId& other) const {
        return _id == other._id && _name == other._name;
    }

    bool operator!=(const SymmetricKeyId& other) const {
        return !(*this == other);
    }

    const boost::optional<id_type>& id() const {
        return _id;
    }

    const std::string& name() const {
        return _name;
    }

private:
    std::string _initStrRep() const;

    boost::optional<id_type> _id;
    std::string _name;
    std::string _strRep;
};

/**
 * Class representing a symmetric key
 */
class SymmetricKey {
    SymmetricKey(const SymmetricKey&) = delete;
    SymmetricKey& operator=(const SymmetricKey&) = delete;

public:
    SymmetricKey(const uint8_t* key,
                 size_t keySize,
                 uint32_t algorithm,
                 SymmetricKeyId keyId,
                 uint32_t initializationCount);
    SymmetricKey(SecureVector<uint8_t> key, uint32_t algorithm, SymmetricKeyId keyId);

    SymmetricKey(SymmetricKey&&);
    SymmetricKey& operator=(SymmetricKey&&);

    ~SymmetricKey() = default;

    int getAlgorithm() const {
        return _algorithm;
    }

    size_t getKeySize() const {
        return _keySize;
    }

    // Return the number of times the key has been retrieved from the key store
    uint32_t getInitializationCount() const {
        return _initializationCount;
    }

    uint32_t incrementAndGetInitializationCount() {
        _initializationCount++;
        return _initializationCount;
    }

    uint64_t getAndIncrementInvocationCount() const {
        return _invocationCount.fetchAndAdd(1);
    }

    const uint8_t* getKey() const {
        return _key->data();
    }

    const SymmetricKeyId& getKeyId() const {
        return _keyId;
    }

    void setKeyId(SymmetricKeyId keyId) {
        _keyId = std::move(keyId);
    }

private:
    int _algorithm;

    size_t _keySize;

    SecureVector<uint8_t> _key;

    SymmetricKeyId _keyId;

    uint32_t _initializationCount;
    mutable AtomicWord<unsigned long long> _invocationCount;
};

using UniqueSymmetricKey = std::unique_ptr<SymmetricKey>;
}  // namespace mongo
