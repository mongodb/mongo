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

#include "mongo/db/index_names.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/string_map.h"

#include <utility>

#include <absl/container/node_hash_map.h>

namespace mongo {

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
        StringData fieldName(e.fieldNameStringData());
        if (e.type() == BSONType::string) {
            indexTypeStr = e.String();
        } else if (WildcardNames::isWildcardFieldName(fieldName)) {
            if (keyPattern.firstElement().type() == BSONType::string &&
                keyPattern.firstElement().fieldNameStringData() == "columnstore"_sd) {
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
IndexType IndexNames::nameToType(StringData accessMethod) {
    auto typeIt = kIndexNameToType.find(accessMethod);
    if (typeIt == kIndexNameToType.end()) {
        return INDEX_BTREE;
    }
    return typeIt->second;
}

}  // namespace mongo
