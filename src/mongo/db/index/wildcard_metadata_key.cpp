/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/index/wildcard_metadata_key.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

FieldRef decodeWildcardMultikeyMetadataPath(const BSONObj& keyBson) {
    BSONObjIterator iter(keyBson);
    while (iter.more()) {
        const auto elem = iter.next();
        if (elem.type() == BSONType::minKey) {
            continue;
        }
        tassert(7354603,
                "An int value must follow MinKey values in a metadata key of a wildcard index.",
                elem.isNumber());
        tassert(7354604,
                "The int value '1' must follow MinKey values in a metadata key of a wildcard "
                "index.",
                elem.numberInt() == 1);
        tassert(7354605,
                "A string value must follow an int value in a metadata key of a wildcard index",
                iter.more());
        const auto nextElem = iter.next();
        tassert(7354606,
                "A string value must follow an int value in a metadata key of a wildcard index",
                nextElem.type() == BSONType::string);
        return FieldRef(nextElem.valueStringData());
    }
    tasserted(7354607,
              str::stream() << "Unexpected format of a metadata key of a wildcard index: "
                            << keyBson);
}

std::set<FieldRef> extractWildcardMultikeyPathsFromMetadataKeys(const KeyStringSet& metadataKeys,
                                                                const Ordering& ordering) {
    std::set<FieldRef> paths;
    for (const auto& key : metadataKeys) {
        BSONObj keyBson = key_string::toBsonSafe(key.getView(), ordering, key.getTypeBits());
        paths.insert(decodeWildcardMultikeyMetadataPath(keyBson));
    }
    return paths;
}

}  // namespace mongo
