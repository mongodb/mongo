
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

#include "mongo/platform/basic.h"

#include "mongo/db/index/wildcard_access_method.h"

#include "mongo/db/catalog/index_catalog_entry.h"

namespace mongo {

WildcardAccessMethod::WildcardAccessMethod(IndexCatalogEntry* wildcardState,
                                           SortedDataInterface* btree)
    : AbstractIndexAccessMethod(wildcardState, btree),
      _keyGen(
          _descriptor->keyPattern(), _descriptor->pathProjection(), _btreeState->getCollator()) {}

bool WildcardAccessMethod::shouldMarkIndexAsMultikey(const BSONObjSet& keys,
                                                     const BSONObjSet& multikeyMetadataKeys,
                                                     const MultikeyPaths& multikeyPaths) const {
    return !multikeyMetadataKeys.empty();
}

void WildcardAccessMethod::doGetKeys(const BSONObj& obj,
                                     BSONObjSet* keys,
                                     BSONObjSet* multikeyMetadataKeys,
                                     MultikeyPaths* multikeyPaths) const {
    _keyGen.generateKeys(obj, keys, multikeyMetadataKeys);
}

std::set<FieldRef> WildcardAccessMethod::getMultikeyPathSet(OperationContext* opCtx) const {
    auto cursor = newCursor(opCtx);
    // All of the keys storing multikeyness metadata are prefixed by a value of 1. Establish an
    // index cursor which will scan this range.
    const BSONObj metadataKeyRangeBegin = BSON("" << 1 << "" << MINKEY);
    const BSONObj metadataKeyRangeEnd = BSON("" << 1 << "" << MAXKEY);

    constexpr bool inclusive = true;
    cursor->setEndPosition(metadataKeyRangeEnd, inclusive);
    auto entry = cursor->seek(metadataKeyRangeBegin, inclusive);

    // Iterate the cursor, copying the multikey paths into an in-memory set.
    std::set<FieldRef> multikeyPaths{};
    while (entry) {
        // Validate that the key contains the expected RecordId.
        invariant(entry->loc.isReserved());
        invariant(entry->loc.repr() ==
                  static_cast<int64_t>(RecordId::ReservedId::kWildcardMultikeyMetadataId));

        // Validate that the first piece of the key is the integer 1.
        BSONObjIterator iter(entry->key);
        invariant(iter.more());
        const auto firstElem = iter.next();
        invariant(firstElem.isNumber());
        invariant(firstElem.numberInt() == 1);
        invariant(iter.more());

        // Extract the path from the second piece of the key.
        const auto secondElem = iter.next();
        invariant(!iter.more());
        invariant(secondElem.type() == BSONType::String);
        multikeyPaths.emplace(secondElem.valueStringData());

        entry = cursor->next();
    }

    return multikeyPaths;
}

}  // namespace mongo
