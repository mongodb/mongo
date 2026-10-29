// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/temp_collections_cleanup_mongod.h"

#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace {

const auto tempCleanupDecoration =
    ServiceContext::declareDecoration<TempCollectionsCleanupMongoD>();

const ReplicaSetAwareServiceRegistry::Registerer<TempCollectionsCleanupMongoD>
    tempCleanupRegisterer("TempCollectionsCleanupMongoD");

}  // namespace

TempCollectionsCleanupMongoD* TempCollectionsCleanupMongoD::get(ServiceContext* sc) {
    return &tempCleanupDecoration(sc);
}

void TempCollectionsCleanupMongoD::onStepUpComplete(OperationContext* opCtx, long long) {
    WriteBlockBypass::get(opCtx).set(true);

    Lock::GlobalLock globalX(opCtx, MODE_IX);

    auto dbNames = catalog::listDatabases();
    for (const auto& dbName : dbNames) {
        if (dbName == DatabaseName::kLocal) {
            continue;
        }

        LOGV2_DEBUG(11336200, 2, "Dropping temporary collections on step-up", logAttrs(dbName));

        Lock::DBLock dbLock(
            opCtx,
            dbName,
            MODE_IX,
            Date_t::max(),
            Lock::DBLockSkipOptions{false /*skipFlowControl*/,
                                    false /*skipRSTLLock*/,
                                    false /*skipShardRegistry*/,
                                    rss::consensus::IntentRegistry::Intent::LocalWrite});

        clearTempCollections(opCtx, dbName);
    }
}

}  // namespace mongo
