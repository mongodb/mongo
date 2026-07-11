// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_hash.h"

#include "mongo/db/basic_types.h"

#include <type_traits>
#include <variant>

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
size_t hash_value(const QueryKnobId& id) {
    return boost::hash_value(id.value);
}

size_t hash_value(const QueryKnobValue& val) {
    size_t seed = boost::hash_value(val.index());
    std::visit(
        [&seed](const auto& v) {
            using V = std::decay_t<decltype(v)>;
            if constexpr (!std::is_same_v<V, DeleteQueryKnobOverride>) {
                boost::hash_combine(seed, v);
            }
        },
        val);
    return seed;
}

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
size_t hash_value(const QuerySettingsKnobOverrides::Entry& e) {
    size_t seed = 0;
    boost::hash_combine(seed, e.id);
    boost::hash_combine(seed, e.value);
    return seed;
}

size_t hash_value(const QuerySettingsKnobOverrides& overrides) {
    auto e = overrides.entries();
    return boost::hash_range(e.begin(), e.end());
}

size_t hash_value(const IndexHintSpec& v) {
    const auto& indexes = v.getAllowedIndexes();
    size_t hash = boost::hash_range(indexes.begin(), indexes.end());
    boost::hash_combine(hash, v.getNs());
    return hash;
}

size_t hash_value(const QuerySettings& querySettings) {
    // The 'serialization_context', 'comment', and 'maxTimeMS' fields are not significant.
    // 'maxTimeMS' affects operation deadline, not plan selection.
    static_assert(QuerySettings::fieldNames.size() == 7,
                  "A new field has been added to the QuerySettings structure, adjust the hash "
                  "function accordingly");

    size_t hash = 0;
    boost::hash_combine(hash, querySettings.getQueryFramework());
    boost::hash_combine(hash, querySettings.getIndexHints());
    boost::hash_combine(hash, querySettings.getReject());
    boost::hash_combine(hash, querySettings.getQueryKnobs());
    return hash;
}

// Alias for existing usage.
size_t hash(const QuerySettings& querySettings) {
    return hash_value(querySettings);
}


}  // namespace mongo::query_settings
