/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_settings/query_settings_hash.h"

#include "mongo/db/basic_types.h"

#include <absl/container/inlined_vector.h>
#include <boost/container_hash/hash.hpp>


namespace absl {

// Add support for boost::hash of absl InlinedVector.
template <typename T, size_t N, typename A>
size_t hash_value(const InlinedVector<T, N, A>& v) {
    return boost::hash_range(v.begin(), v.end());
}
}  // namespace absl

namespace boost {
// Reproduction of impl for std::optional from boost container_hash/hash.hpp,
// as there is no impl for boost::optional in the current boost version.
template <typename T>
std::size_t hash_value(const optional<T>& v) {
    if (!v.has_value()) {
        // Arbitrary value for empty optional.
        return 0x12345678;
    } else {
        hash<T> hf;
        return hf(*v);
    }
}
}  // namespace boost

namespace mongo {
size_t hash_value(const OptionalBool& v) {
    // OptionalBool hash needs to be consistent with equality. OptionalBool currently relies on
    // implicit conversion to bool for ==. Thus, OptionalBool() == OptionalBool(false).
    // Thus, it is required that hash(OptionalBool()) == hash(OptionalBool(false));
    boost::hash<bool> hf;
    return hf(bool(v));
}

size_t hash_value(const NamespaceSpec& ns) {
    size_t hash = 0;
    boost::hash_combine(hash, ns.getDb());
    boost::hash_combine(hash, ns.getColl());
    return hash;
}
}  // namespace mongo

namespace mongo::query_settings {
size_t hash_value(const IndexHintSpec& v) {
    const auto& indexes = v.getAllowedIndexes();
    size_t hash = boost::hash_range(indexes.begin(), indexes.end());
    boost::hash_combine(hash, v.getNs());
    return hash;
}

size_t hash_value(const QuerySettings& querySettings) {
    // The 'serialization_context' and 'comment' fields are not significant.
    static_assert(QuerySettings::fieldNames.size() == 5,
                  "A new field has been added to the QuerySettings structure, adjust the hash "
                  "function accordingly");

    size_t hash = 0;
    boost::hash_combine(hash, querySettings.getQueryFramework());
    boost::hash_combine(hash, querySettings.getIndexHints());
    boost::hash_combine(hash, querySettings.getReject());
    return hash;
}

// Alias for existing usage.
size_t hash(const QuerySettings& querySettings) {
    return hash_value(querySettings);
}


}  // namespace mongo::query_settings
