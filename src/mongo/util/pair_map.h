/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

template <typename T>
class PairMapTraits {
public:
    using OwnedType = T;
    using ViewType = T;
    using HashableType = T;

    static const T& toOwned(const T& t) {
        return t;
    }
    static const T& toView(const T& t) {
        return t;
    }
    static const T& toHashable(const T& t) {
        return t;
    }
};

template <>
class PairMapTraits<StringData> {
public:
    using OwnedType = std::string;
    using ViewType = StringData;
    using HashableType = absl::string_view;

    static std::string toOwned(const StringData& t) {
        return t.toString();
    }
    static const StringData& toView(const StringData& t) {
        return t;
    }
    static absl::string_view toHashable(const StringData& t) {
        // Use the default absl string hasher.
        return absl::string_view(t.rawData(), t.size());
    }
};

template <>
class PairMapTraits<std::string> {
public:
    using OwnedType = std::string;
    using ViewType = StringData;
    using HashableType = absl::string_view;

    static const std::string& toOwned(const std::string& t) {
        return t;
    }
    static StringData toView(const std::string& t) {
        return StringData(t);
    }
    static absl::string_view toHashable(const std::string& t) {
        // Use the default absl string hasher.
        return absl::string_view(t.data(), t.size());
    }
};

/**
 * Type that bundles a hashed key with the actual pair value so that hashing can be performed
 * outside of insert() call. This is needed to facilitate heterogeneous lookup.
 */
template <typename T1, typename T2>
class PairMapHashedKey {
public:
    using OwnedType1 = typename PairMapTraits<T1>::OwnedType;
    using OwnedType2 = typename PairMapTraits<T2>::OwnedType;
    using OwnedPairType = std::pair<OwnedType1, OwnedType2>;
    using ViewType1 = typename PairMapTraits<T1>::ViewType;
    using ViewType2 = typename PairMapTraits<T2>::ViewType;
    using ViewPairType = std::pair<ViewType1, ViewType2>;

    explicit PairMapHashedKey(ViewPairType key, std::size_t hash)
        : _key(std::move(key)), _hash(hash) {}

    explicit operator OwnedPairType() const {
        return {PairMapTraits<ViewType1>::toOwned(_key.first),
                PairMapTraits<ViewType2>::toOwned(_key.second)};
    }

    const ViewPairType& key() const {
        return _key;
    }

    std::size_t hash() const {
        return _hash;
    }

private:
    ViewPairType _key;
    std::size_t _hash;
};

/**
 * Hasher to support heterogeneous lookup.
 */
template <typename T1, typename T2>
class PairMapHasher {
public:
    using OwnedType1 = typename PairMapTraits<T1>::OwnedType;
    using OwnedType2 = typename PairMapTraits<T2>::OwnedType;
    using OwnedPairType = std::pair<OwnedType1, OwnedType2>;
    using ViewType1 = typename PairMapTraits<T1>::ViewType;
    using ViewType2 = typename PairMapTraits<T2>::ViewType;
    using ViewPairType = std::pair<ViewType1, ViewType2>;
    using HashableType1 = typename PairMapTraits<T1>::HashableType;
    using HashableType2 = typename PairMapTraits<T2>::HashableType;
    using HashablePairType = std::pair<HashableType1, HashableType2>;

    // This using directive activates heterogeneous lookup in the hash table.
    using is_transparent = void;

    std::size_t operator()(const ViewPairType& sd) const {
        return absl::Hash<HashablePairType>{}(
            std::make_pair(PairMapTraits<ViewType1>::toHashable(sd.first),
                           PairMapTraits<ViewType2>::toHashable(sd.second)));
    }

    std::size_t operator()(const OwnedPairType& s) const {
        return operator()(std::make_pair(PairMapTraits<OwnedType1>::toView(s.first),
                                         PairMapTraits<OwnedType2>::toView(s.second)));
    }

    std::size_t operator()(const PairMapHashedKey<T1, T2>& key) const {
        return key.hash();
    }

    PairMapHashedKey<T1, T2> hashed_key(const ViewPairType& sd) const {
        return PairMapHashedKey<T1, T2>(sd, operator()(sd));
    }
};

template <typename T1, typename T2>
class PairMapEq {
public:
    using ViewType1 = typename PairMapTraits<T1>::ViewType;
    using ViewType2 = typename PairMapTraits<T2>::ViewType;
    using ViewPairType = std::pair<ViewType1, ViewType2>;

    // This using directive activates heterogeneous lookup in the hash table.
    using is_transparent = void;

    bool operator()(const ViewPairType& lhs, const ViewPairType& rhs) const {
        return lhs == rhs;
    }

    bool operator()(const PairMapHashedKey<T1, T2>& lhs, const ViewPairType& rhs) const {
        return lhs.key() == rhs;
    }

    bool operator()(const ViewPairType& lhs, const PairMapHashedKey<T1, T2>& rhs) const {
        return lhs == rhs.key();
    }

    bool operator()(const PairMapHashedKey<T1, T2>& lhs,
                    const PairMapHashedKey<T1, T2>& rhs) const {
        return lhs.key() == rhs.key();
    }
};

template <typename K1, typename K2, typename V>
using PairMap = absl::flat_hash_map<std::pair<K1, K2>, V, PairMapHasher<K1, K2>, PairMapEq<K1, K2>>;

template <typename K1, typename K2>
using PairSet = absl::flat_hash_set<std::pair<K1, K2>, PairMapHasher<K1, K2>, PairMapEq<K1, K2>>;

}  // namespace mongo
