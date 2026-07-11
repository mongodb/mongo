// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/requires_index_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

RequiresIndexStage::RequiresIndexStage(std::string_view stageType,
                                       ExpressionContext* expCtx,
                                       CollectionAcquisition collection,
                                       const IndexCatalogEntry* indexEntry,
                                       WorkingSet* workingSet)
    : RequiresCollectionStage(stageType, expCtx, collection),
      _entry(indexEntry),
      _indexIdent(_entry->getIdent()),
      _indexName(_entry->descriptor()->indexName()),
      _workingSetIndexId(workingSet->registerIndexIdent(_indexIdent)) {}


void RequiresIndexStage::doSaveStateRequiresCollection() {
    doSaveStateRequiresIndex();

    // Set the index entry to null, since accessing this pointer is illegal during yield.
    _entry = nullptr;
}

void RequiresIndexStage::doRestoreStateRequiresCollection() {
    const auto indexCatalog = collectionPtr()->getIndexCatalog();
    auto indexEntry = indexCatalog->findIndexByIdent(expCtx()->getOperationContext(), _indexIdent);
    uassert(ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: index '" << _indexName << "' dropped",
            indexEntry);

    // Re-obtain the index entry pointer that was set to null during yield preparation. It is safe
    // to access the index entry when the query is active, as its validity is protected by at least
    // MODE_IS collection locks; or, in the case of lock-free reads, its lifetime is managed by the
    // CollectionCatalog stashed on the RecoveryUnit snapshot, which is kept alive until the query
    // yields.
    _entry = indexEntry;

    doRestoreStateRequiresIndex();
}

}  // namespace mongo
