/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/temp_collections_cleanup_mongod.h"

#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/user_write_block/write_block_bypass.h"
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
