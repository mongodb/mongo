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

#include <string>

#include "mongo/base/string_data.h"

namespace mongo {

class BSONObj;

/**
 * We need to know what 'type' an index is in order to plan correctly.
 */
enum IndexType {
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
};

/**
 * We use the std::string representation of index names all over the place, so we declare them all
 * once here.
 */
class IndexNames {
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
    static IndexType nameToType(StringData accessMethod);
};

/**
 * Contain utilities to work with wildcard fields used for Wildcard indexes and Columnstore.
 */
struct WildcardNames {
    static constexpr StringData WILDCARD_FIELD_NAME = "$**"_sd;
    static constexpr StringData WILDCARD_FIELD_NAME_SUFFIX = ".$**"_sd;

    inline static bool isWildcardFieldName(StringData fieldName) {
        return fieldName == WILDCARD_FIELD_NAME || fieldName.endsWith(WILDCARD_FIELD_NAME_SUFFIX);
    }
};

}  // namespace mongo
