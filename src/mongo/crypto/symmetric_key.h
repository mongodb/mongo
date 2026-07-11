// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/secure_allocator.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
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

    template <typename H>
    friend H AbslHashValue(H h, const SymmetricKeyId& keyid) {
        return H::combine(std::move(h), keyid.toString());
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
    mutable Atomic<unsigned long long> _invocationCount;
};

using UniqueSymmetricKey = std::unique_ptr<SymmetricKey>;
}  // namespace mongo
