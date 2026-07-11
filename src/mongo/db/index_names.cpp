// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_names.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/string_map.h"

#include <string_view>
#include <utility>

#include <absl/container/node_hash_map.h>

namespace mongo {
using namespace std::literals::string_view_literals;

std::string toString(IndexType indexType) {
    switch (indexType) {
        case INDEX_COLUMN:
            return IndexNames::COLUMN;
        case INDEX_2D:
            return IndexNames::GEO_2D;
        case INDEX_ENCRYPTED_RANGE:
            return IndexNames::ENCRYPTED_RANGE;
        case INDEX_HAYSTACK:
            return IndexNames::GEO_HAYSTACK;
        case INDEX_2DSPHERE:
            return IndexNames::GEO_2DSPHERE;
        case INDEX_2DSPHERE_BUCKET:
            return IndexNames::GEO_2DSPHERE_BUCKET;
        case INDEX_TEXT:
            return IndexNames::TEXT;
        case INDEX_HASHED:
            return IndexNames::HASHED;
        case INDEX_WILDCARD:
            return IndexNames::WILDCARD;
        case INDEX_BTREE:
            // While `IndexNames::BTREE` is represented internally as an empty string, this function
            // represents `IndexType::INDEX_BTREE` as "btree" to make logs more readable.
            return "btree";
        case INDEX_TYPE_COUNT:
            return "index_type_count";
        default:
            MONGO_UNREACHABLE;
    }
}

using std::string;

const string IndexNames::GEO_2D = "2d";
const string IndexNames::GEO_2DSPHERE = "2dsphere";
const string IndexNames::GEO_2DSPHERE_BUCKET = "2dsphere_bucket";
const string IndexNames::TEXT = "text";
const string IndexNames::HASHED = "hashed";
const string IndexNames::BTREE = "";
const string IndexNames::WILDCARD = "wildcard";
// We no longer support column store indexes. We use this value to reject creating them.
const string IndexNames::COLUMN = "columnstore";
// Encrypted range indexes are "pseudo-indexes", and as such, they cannot be created.
const string IndexNames::ENCRYPTED_RANGE = "queryable_encrypted_range";
// We no longer support geo haystack indexes. We use this value to reject creating them.
const string IndexNames::GEO_HAYSTACK = "geoHaystack";

const StringMap<IndexType> kIndexNameToType = {
    {IndexNames::GEO_2D, INDEX_2D},
    {IndexNames::GEO_HAYSTACK, INDEX_HAYSTACK},
    {IndexNames::GEO_2DSPHERE, INDEX_2DSPHERE},
    {IndexNames::GEO_2DSPHERE_BUCKET, INDEX_2DSPHERE_BUCKET},
    {IndexNames::TEXT, INDEX_TEXT},
    {IndexNames::HASHED, INDEX_HASHED},
    {IndexNames::COLUMN, INDEX_COLUMN},
    {IndexNames::ENCRYPTED_RANGE, INDEX_ENCRYPTED_RANGE},
    {IndexNames::WILDCARD, INDEX_WILDCARD},
};

// static
string IndexNames::findPluginName(const BSONObj& keyPattern) {
    BSONObjIterator i(keyPattern);
    string indexTypeStr = "";
    while (i.more()) {
        BSONElement e = i.next();
        std::string_view fieldName(e.fieldNameStringData());
        if (e.type() == BSONType::string) {
            indexTypeStr = e.String();
        } else if (WildcardNames::isWildcardFieldName(fieldName)) {
            if (keyPattern.firstElement().type() == BSONType::string &&
                keyPattern.firstElement().fieldNameStringData() == "columnstore"sv) {
                return IndexNames::COLUMN;
            } else {
                // Returns IndexNames::WILDCARD directly here because we rely on the caller to
                // validate that wildcard indexes cannot compound with other special index type.
                return IndexNames::WILDCARD;
            }
        } else
            continue;
    }

    return indexTypeStr.empty() ? IndexNames::BTREE : indexTypeStr;
}

// static
bool IndexNames::isKnownName(const string& name) {
    return (name == IndexNames::BTREE || kIndexNameToType.count(name));
}

// static
IndexType IndexNames::nameToType(std::string_view accessMethod) {
    auto typeIt = kIndexNameToType.find(accessMethod);
    if (typeIt == kIndexNameToType.end()) {
        return INDEX_BTREE;
    }
    return typeIt->second;
}

// static
bool IndexNames::isVirtualIndexType(const std::string& name) {
    if (isKnownName(name)) {
        switch (nameToType(name)) {
            case INDEX_ENCRYPTED_RANGE:
                return true;
            default:
                return false;
        }
    }
    return false;
}

}  // namespace mongo
