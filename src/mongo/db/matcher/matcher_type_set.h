// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

using findBSONTypeAliasFun = std::function<boost::optional<BSONType>(std::string_view)>;

/**
 * Represents a set of types or of type aliases in the match language. The set consists of the BSON
 * types as well as "number", which is an alias for all numeric BSON types (NumberInt, NumberLong,
 * and so on).
 */
struct MatcherTypeSet {
    static constexpr std::string_view kMatchesAllNumbersAlias = "number"sv;

    // Maps from the set of JSON Schema primitive types to the corresponding BSON types. Excludes
    // "number" since this alias maps to a set of BSON types, and "integer" since it is not
    // supported.
    static const StringMap<BSONType> kJsonSchemaTypeAliasMap;

    static boost::optional<BSONType> findJsonSchemaTypeAlias(std::string_view key);

    /**
     * Given a set of string type alias and a mapping from string alias to BSON type, returns the
     * corresponding MatcherTypeSet.
     *
     * Returns an error if any of the string aliases are unknown.
     */
    static StatusWith<MatcherTypeSet> fromStringAliases(std::set<std::string_view> typeAliases,
                                                        const findBSONTypeAliasFun& aliasMapFind);

    /**
     * Constructs an empty type set.
     */
    MatcherTypeSet() = default;

    /* implicit */ MatcherTypeSet(BSONType bsonType) : bsonTypes({bsonType}) {}

    /**
     * Returns true if 'bsonType' is present in the set.
     */
    bool hasType(BSONType bsonType) const {
        return (allNumbers && isNumericBSONType(bsonType)) ||
            bsonTypes.find(bsonType) != bsonTypes.end();
    }

    /**
     * Returns true if this set contains a single type or type alias. For instance, returns true if
     * the set is {"number"} or {"int"}, but not if the set is empty or {"number", "string"}.
     */
    bool isSingleType() const {
        return (allNumbers && bsonTypes.empty()) || (!allNumbers && bsonTypes.size() == 1u);
    }

    bool isEmpty() const {
        return !allNumbers && bsonTypes.empty();
    }

    /**
     * Returns a bitmask representing the set of BSONTypes that this MatcherTypeSet contains.
     *
     * For details on how these bitmasks are encoded, see mongo::getBSONTypeMask().
     */
    uint32_t getBSONTypeMask() const;

    void toBSONArray(BSONArrayBuilder* builder) const;

    BSONArray toBSONArray() const {
        BSONArrayBuilder builder;
        toBSONArray(&builder);
        return builder.arr();
    }

    bool operator==(const MatcherTypeSet& other) const {
        return allNumbers == other.allNumbers && bsonTypes == other.bsonTypes;
    }

    bool operator!=(const MatcherTypeSet& other) const {
        return !(*this == other);
    }

    bool allNumbers = false;
    std::set<BSONType> bsonTypes;
};

/**
 * Adds the type represented by 'typeAlias' to 'typeSet', using 'aliasMap' as the mapping from
 * string to BSON type.
 *
 * Returns a non-OK status if 'typeAlias' does not represent a valid type.
 */
Status addAliasToTypeSet(std::string_view typeAlias,
                         const findBSONTypeAliasFun& aliasMapFind,
                         MatcherTypeSet* typeSet);
}  // namespace mongo
