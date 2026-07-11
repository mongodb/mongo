// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/requires_collection_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/assert_util.h"


namespace mongo {

void RequiresCollectionStage::doSaveState() {
    doSaveStateRequiresCollection();
}

void RequiresCollectionStage::doRestoreState(const RestoreContext& context) {
    if (context.type() == RestoreContext::RestoreType::kExternal) {
        // If we restore externally and get a null Collection we need to figure out if this was a
        // drop or rename. The external lookup could have been done for UUID or namespace.
        const auto& coll = _collection.getCollectionPtr();
        _collectionPtr = &coll;

        // If collection exists uuid does not match assume lookup was over namespace and treat this
        // as a drop.
        if (coll && coll->uuid() != _collectionUUID) {
            PlanYieldPolicy::throwCollectionDroppedError(_collectionUUID);
        }

        // If we didn't get a valid collection but can still find the UUID in the catalog then we
        // treat this as a rename.
        if (!coll) {
            auto catalog = CollectionCatalog::get(opCtx());
            auto newNss = catalog->lookupNSSByUUID(opCtx(), _collectionUUID);
            if (newNss && *newNss != _nss) {
                PlanYieldPolicy::throwCollectionRenamedError(_nss, *newNss, _collectionUUID);
            }
        }
    }

    const auto& coll = *_collectionPtr;

    if (!coll) {
        PlanYieldPolicy::throwCollectionDroppedError(_collectionUUID);
    }

    // TODO SERVER-31695: Allow queries to survive collection rename, rather than throwing here
    // when a rename has happened during yield.
    if (const auto& newNss = coll->ns(); newNss != _nss) {
        PlanYieldPolicy::throwCollectionRenamedError(_nss, newNss, _collectionUUID);
    }

    uassert(ErrorCodes::QueryPlanKilled,
            "the catalog was closed and reopened",
            getCatalogEpoch() == _catalogEpoch);

    if (expCtx()->getQueryKnobConfiguration().getEnablePathArrayness()) {
        if (auto current = CollectionQueryInfo::get(coll).getPathArrayness()) {
            _pathArraynessChecker.uassertIfInvalidatedAndSyncEpoch(*current, coll->ns());
        }
    }

    doRestoreStateRequiresCollection();
}

uint64_t RequiresCollectionStage::getCatalogEpoch() const {
    return CollectionCatalog::get(opCtx())->getEpoch();
}

}  // namespace mongo
