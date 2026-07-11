// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class BSONObj;

/**
 * We need to know what 'type' an index is in order to plan correctly.
 */
enum [[MONGO_MOD_PUBLIC]] IndexType {
    INDEX_BTREE,
    INDEX_COLUMN,
    INDEX_2D,
    INDEX_ENCRYPTED_RANGE,
    INDEX_HAYSTACK,
    INDEX_2DSPHERE,
    INDEX_2DSPHERE_BUCKET,
    INDEX_TEXT,
    INDEX_HASHED,
    INDEX_WILDCARD,
    INDEX_TYPE_COUNT,  // Count of IndexType, not a index
};

/**
 * Converts an IndexType to a string.
 *
 * This function is used strictly for logging and makes no assumptions about which `IndexType`s
 * are valid.
 */
std::string toString(IndexType indexType);

/**
 * We use the std::string representation of index names all over the place, so we declare them all
 * once here.
 */
class [[MONGO_MOD_PUBLIC]] IndexNames {
public:
    static const std::string BTREE;
    static const std::string GEO_2D;
    static const std::string GEO_2DSPHERE;
    static const std::string GEO_2DSPHERE_BUCKET;
    static const std::string GEO_HAYSTACK;
    static const std::string HASHED;
    static const std::string TEXT;
    static const std::string WILDCARD;
    static const std::string COLUMN;
    static const std::string ENCRYPTED_RANGE;

    /**
     * Return the first std::string value in the provided object.  For an index key pattern,
     * a field with a non-string value indicates a "special" (not straight Btree) index.
     */
    static std::string findPluginName(const BSONObj& keyPattern);

    /**
     * Is the provided access method name one we recognize?
     */
    static bool isKnownName(const std::string& name);

    /**
     * Convert an index name to an IndexType.
     */
    static IndexType nameToType(std::string_view accessMethod);

    /**
     * Index is not intended to be user facing.
     */
    static bool isVirtualIndexType(const std::string& name);
};

/**
 * Contain utilities to work with wildcard fields used for Wildcard indexes.
 */
struct [[MONGO_MOD_PUBLIC]] WildcardNames {
    static constexpr std::string_view WILDCARD_FIELD_NAME = "$**"sv;
    static constexpr std::string_view WILDCARD_FIELD_NAME_SUFFIX = ".$**"sv;

    inline static bool isWildcardFieldName(std::string_view fieldName) {
        return fieldName == WILDCARD_FIELD_NAME || fieldName.ends_with(WILDCARD_FIELD_NAME_SUFFIX);
    }
};

}  // namespace mongo
