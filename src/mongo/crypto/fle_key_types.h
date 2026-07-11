// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>

#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {

using KeyMaterial = SecureVector<std::uint8_t>;

// u = [1, max parallel clients)
using FLEContentionFactor = std::uint64_t;
using FLECounter = std::uint64_t;

/**
 * There are two types of keys that are user supplied.
 * 1. Index, aka S - this encrypts the index structures
 * 2. User, aka K - this encrypts the user data.
 *
 * These keys only exist on the client, they are never on the server-side.
 */
enum class FLEKeyType {
    Index,  // i.e. S
    User,   // i.e. K
};

/**
 * Template class to ensure unique C++ types for each key.
 */
template <FLEKeyType KeyT>
struct FLEKey {
    static constexpr std::size_t kFieldLevelEncryptionKeySize = 96;

    FLEKey() = default;

    FLEKey(KeyMaterial dataIn) : data(std::move(dataIn)) {

        // This is not a mistake; same keys will be used in FLE2 as in FLE1
        uassert(6364500,
                fmt::format("Length of KeyMaterial is expected to be {} bytes, found {}",
                            kFieldLevelEncryptionKeySize,
                            data->size()),
                data->size() == kFieldLevelEncryptionKeySize);
    }

    ConstDataRange toCDR() const {
        return ConstDataRange(data->data(), data->data() + data->size());
    }

    // Actual type of the key
    FLEKeyType type{KeyT};

    // Raw bytes of the key
    KeyMaterial data;
};

using FLEIndexKey = FLEKey<FLEKeyType::Index>;
using FLEUserKey = FLEKey<FLEKeyType::User>;

/**
 * Key Material and its UUID id.
 *
 * The UUID is persisted into the serialized structures so that decryption is self-describing.
 */
template <FLEKeyType KeyT>
struct FLEKeyAndId {

    FLEKeyAndId(KeyMaterial material, UUID uuid) : key(material), keyId(uuid) {}

    FLEKey<KeyT> key;
    UUID keyId;
};

using FLEIndexKeyAndId = FLEKeyAndId<FLEKeyType::Index>;
using FLEUserKeyAndId = FLEKeyAndId<FLEKeyType::User>;

}  // namespace mongo
