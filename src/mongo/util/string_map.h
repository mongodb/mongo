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

#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>
#include <absl/strings/string_view.h>
#include <cstddef>
#include <string>
#include <type_traits>

#include "mongo/base/string_data.h"
#include "mongo/stdx/trusted_hasher.h"
#include "mongo/util/assert_util.h"

namespace mongo {

// Type that bundles a hashed key with the actual string so hashing can be performed outside of
// insert call by using heterogeneous lookup.
struct StringMapHashedKey {
public:
    explicit StringMapHashedKey(StringData sd, std::size_t hash) : _sd(sd), _hash(hash) {}

    explicit operator std::string() const {
        return _sd.toString();
    }

    StringData key() const {
        return _sd;
    }

    std::size_t hash() const {
        return _hash;
    }

private:
    StringData _sd;
    std::size_t _hash;
};

// Hasher to support heterogeneous lookup for StringData and string-like elements.
struct StringMapHasher {
    // This using directive activates heterogeneous lookup in the hash table
    using is_transparent = void;

    std::size_t operator()(StringData sd) const {
        // Use the default absl string hasher.
        return absl::Hash<absl::string_view>{}(absl::string_view(sd.rawData(), sd.size()));
    }

    std::size_t operator()(const std::string& s) const {
        return operator()(StringData(s));
    }

    std::size_t operator()(const char* s) const {
        return operator()(StringData(s));
    }

    std::size_t operator()(StringMapHashedKey key) const {
        return key.hash();
    }

    StringMapHashedKey hashed_key(StringData sd) {
        return StringMapHashedKey(sd, operator()(sd));
    }
};

struct StringMapEq {
    // This using directive activates heterogeneous lookup in the hash table
    using is_transparent = void;

    bool operator()(StringData lhs, StringData rhs) const {
        return lhs == rhs;
    }

    bool operator()(StringMapHashedKey lhs, StringData rhs) const {
        return lhs.key() == rhs;
    }

    bool operator()(StringData lhs, StringMapHashedKey rhs) const {
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
using StringDataMap = absl::flat_hash_map<StringData, V, StringMapHasher, StringMapEq>;

using StringDataSet = absl::flat_hash_set<StringData, StringMapHasher, StringMapEq>;

// StringMapHasher is a trusted hasher, no need to wrap in a secondary layer of hashing when used in
// stdx unordered containers.
template <>
struct IsTrustedHasher<StringMapHasher, std::string> : std::true_type {};
template <>
struct IsTrustedHasher<StringMapHasher, StringData> : std::true_type {};

}  // namespace mongo
