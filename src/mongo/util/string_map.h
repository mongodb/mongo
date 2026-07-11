// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>
#include <absl/strings/string_view.h>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

// Type that bundles a hashed key with the actual string so hashing can be performed outside of
// insert call by using heterogeneous lookup.
struct StringMapHashedKey {
public:
    explicit StringMapHashedKey(std::string_view sd, std::size_t hash) : _sd(sd), _hash(hash) {}

    explicit operator std::string() const {
        return std::string{_sd};
    }

    std::string_view key() const {
        return _sd;
    }

    std::size_t hash() const {
        return _hash;
    }

private:
    std::string_view _sd;
    std::size_t _hash;
};

// Hasher to support heterogeneous lookup for std::string_view and string-like elements.
struct StringMapHasher {
    // This using directive activates heterogeneous lookup in the hash table
    using is_transparent = void;

    std::size_t operator()(std::string_view sd) const {
        // Use the default absl string hasher.
        return absl::Hash<absl::string_view>{}(absl::string_view(sd.data(), sd.size()));
    }

    std::size_t operator()(const std::string& s) const {
        return operator()(std::string_view(s));
    }

    std::size_t operator()(const char* s) const {
        return operator()(std::string_view(s));
    }

    std::size_t operator()(StringMapHashedKey key) const {
        return key.hash();
    }

    StringMapHashedKey hashed_key(std::string_view sd) {
        return StringMapHashedKey(sd, operator()(sd));
    }
};

struct StringMapEq {
    // This using directive activates heterogeneous lookup in the hash table
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const {
        return lhs == rhs;
    }

    bool operator()(StringMapHashedKey lhs, std::string_view rhs) const {
        return lhs.key() == rhs;
    }

    bool operator()(std::string_view lhs, StringMapHashedKey rhs) const {
        return lhs == rhs.key();
    }

    bool operator()(StringMapHashedKey lhs, StringMapHashedKey rhs) const {
        return lhs.key() == rhs.key();
    }
};

template <typename V>
using StringMap = absl::flat_hash_map<std::string, V, StringMapHasher, StringMapEq>;

using StringSet = absl::flat_hash_set<std::string, StringMapHasher, StringMapEq>;

template <typename V>
using StringDataMap = absl::flat_hash_map<std::string_view, V, StringMapHasher, StringMapEq>;

using StringDataSet = absl::flat_hash_set<std::string_view, StringMapHasher, StringMapEq>;

// StringMapHasher is a trusted hasher, no need to wrap in a secondary layer of hashing when used in
// stdx unordered containers.
template <>
struct IsTrustedHasher<StringMapHasher, std::string> : std::true_type {};
template <>
struct IsTrustedHasher<StringMapHasher, std::string_view> : std::true_type {};

}  // namespace mongo
