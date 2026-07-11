// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/all_indices_required_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

namespace mongo {

AllIndicesRequiredChecker::AllIndicesRequiredChecker(
    const MultipleCollectionAccessor& collections) {
    saveIndicesForCollection(collections.getMainCollection());
    for (const auto& [_, acq] : collections.getSecondaryCollectionAcquisitions()) {
        saveIndicesForCollection(acq.getCollectionPtr());
    }
}

void AllIndicesRequiredChecker::saveIndicesForCollection(const CollectionPtr& collection) {
    if (collection) {
        auto allEntriesShared =
            collection->getIndexCatalog()->getEntriesShared(IndexCatalog::InclusionPolicy::kReady);
        auto& indexMap = _identEntries[collection->uuid()];
        for (auto&& index : allEntriesShared) {
            indexMap[index->descriptor()->indexName()] = index->getIdent();
        }
    }
}

void AllIndicesRequiredChecker::checkIndicesForCollection(OperationContext* opCtx,
                                                          const CollectionPtr& collection) const {
    tassert(11321000, "collection must not be null", collection);

    auto it = _identEntries.find(collection->uuid());
    tassert(11321001,
            fmt::format("cannot find index idents for collection uuid {}",
                        collection->uuid().toString()),
            it != _identEntries.end());

    for (const auto& [name, ident] : it->second) {
        // Structured bindings cannot be captured by closures (the uassert below).
        auto& nameRef = name;
        auto indexDesc = collection->getIndexCatalog()->findIndexByIdent(opCtx, ident);
        uassert(ErrorCodes::QueryPlanKilled,
                str::stream() << "query plan killed :: index '" << nameRef << "' for collection '"
                              << collection->ns().toStringForErrorMsg() << "' dropped",
                indexDesc);
    }
}

void AllIndicesRequiredChecker::check(OperationContext* opCtx,
                                      const MultipleCollectionAccessor& collections) const {
    checkIndicesForCollection(opCtx, collections.getMainCollection());
    for (const auto& [_, acq] : collections.getSecondaryCollectionAcquisitions()) {
        const auto& collection = acq.getCollectionPtr();
        if (collection) {
            checkIndicesForCollection(opCtx, collection);
        }
    }
}

}  // namespace mongo
