/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/all_indices_required_checker.h"

namespace mongo {

AllIndicesRequiredChecker::AllIndicesRequiredChecker(
    const MultipleCollectionAccessor& collections) {
    saveIndicesForCollection(collections.getMainCollection());
    for (auto& [_, collection] : collections.getSecondaryCollections()) {
        saveIndicesForCollection(collection);
    }
}

void AllIndicesRequiredChecker::saveIndicesForCollection(const CollectionPtr& collection) {
    if (collection) {
        auto allEntriesShared = collection->getIndexCatalog()->getAllReadyEntriesShared();
        auto& indexMap = _identEntries[collection->uuid()];
        for (auto&& index : allEntriesShared) {
            indexMap[index->descriptor()->indexName()] = index->getIdent();
        }
    }
}

void AllIndicesRequiredChecker::checkIndicesForCollection(OperationContext* opCtx,
                                                          const CollectionPtr& collection) const {
    invariant(collection);

    auto it = _identEntries.find(collection->uuid());
    invariant(it != _identEntries.end());

    for (const auto& [name, ident] : it->second) {
        // Structured bindings cannot be captured by closures (the uassert below).
        auto& nameRef = name;
        auto indexDesc = collection->getIndexCatalog()->findIndexByIdent(opCtx, ident);
        uassert(ErrorCodes::QueryPlanKilled,
                str::stream() << "query plan killed :: index '" << nameRef << "' for collection '"
                              << collection->ns().toStringForErrorMsg() << "' dropped",
                indexDesc && !indexDesc->getEntry()->isDropped());
    }
}

void AllIndicesRequiredChecker::check(OperationContext* opCtx,
                                      const MultipleCollectionAccessor& collections) const {
    checkIndicesForCollection(opCtx, collections.getMainCollection());
    for (auto& [_, collection] : collections.getSecondaryCollections()) {
        if (collection) {
            checkIndicesForCollection(opCtx, collection);
        }
    }
}

}  // namespace mongo
