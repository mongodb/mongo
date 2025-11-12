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

#include "mongo/db/index_builds/index_build_test_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"

namespace mongo {
Status createIndex(OperationContext* opCtx, StringData ns, const BSONObj& keys, bool unique) {
    BSONObjBuilder specBuilder;
    specBuilder.append("name", DBClientBase::genIndexName(keys));
    specBuilder.append("key", keys);
    specBuilder.append("v", 2);
    if (unique) {
        specBuilder.appendBool("unique", true);
    }
    return createIndexFromSpec(opCtx, ns, specBuilder.done());
}

Status createIndexFromSpec(OperationContext* opCtx, StringData ns, const BSONObj& spec) {
    return createIndexFromSpec(opCtx, nullptr, ns, spec);
}

Status createIndexFromSpec(OperationContext* opCtx,
                           VectorClockMutable* clock,
                           StringData ns,
                           const BSONObj& spec) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
    invariant(!shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));

    auto nextTimestamp = [&] {
        if (clock) {
            return clock->tickClusterTime(1).asTimestamp();
        }
        return Timestamp(1, 1);
    };

    {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        WriteUnitOfWork wunit(opCtx);
        CollectionWriter writer{opCtx, nss};
        auto coll = writer.getWritableCollection(opCtx);
        if (!coll) {
            auto db = autoDb.ensureDbExists(opCtx);
            invariant(db);
            coll = db->createCollection(opCtx, NamespaceString::createNamespaceString_forTest(ns));
        }
        invariant(coll);
        wunit.commit();
    }

    MultiIndexBlock indexer;
    ScopeGuard abortOnExit([&] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);
        indexer.abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
    });

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    IndexBuildInfo indexBuildInfo(
        spec, *storageEngine, nss.dbName(), VersionContext::getDecoration(opCtx));
    {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);
        Status status = indexer
                            .init(
                                opCtx,
                                collection,
                                {indexBuildInfo},
                                [&] {
                                    if (opCtx->writesAreReplicated() &&
                                        shard_role_details::getRecoveryUnit(opCtx)
                                            ->getCommitTimestamp()
                                            .isNull()) {
                                        uassertStatusOK(shard_role_details::getRecoveryUnit(opCtx)
                                                            ->setTimestamp(nextTimestamp()));
                                    }
                                },
                                MultiIndexBlock::InitMode::SteadyState,
                                boost::none,
                                /*generateTableWrites=*/true)
                            .getStatus();
        if (status == ErrorCodes::IndexAlreadyExists) {
            return Status::OK();
        }
        if (!status.isOK()) {
            return status;
        }
    }

    if (auto status = indexer.insertAllDocumentsInCollection(opCtx, nss); !status.isOK()) {
        return status;
    }

    AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
    Lock::CollectionLock collLock(opCtx, nss, MODE_X);
    CollectionWriter collection(opCtx, nss);
    if (auto status = indexer.retrySkippedRecords(opCtx, collection.get()); !status.isOK()) {
        return status;
    }
    if (auto status = indexer.checkConstraints(opCtx, collection.get()); !status.isOK()) {
        return status;
    }
    WriteUnitOfWork wunit(opCtx);
    if (auto status = indexer.commit(opCtx,
                                     collection.getWritableCollection(opCtx),
                                     MultiIndexBlock::kNoopOnCreateEachFn,
                                     MultiIndexBlock::kNoopOnCommitFn);
        !status.isOK()) {
        return status;
    }
    if (opCtx->writesAreReplicated()) {
        if (auto status = shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(nextTimestamp());
            !status.isOK()) {
            return status;
        }
    }
    wunit.commit();
    abortOnExit.dismiss();
    return Status::OK();
}

Status initializeMultiIndexBlock(OperationContext* opCtx,
                                 CollectionWriter& collection,
                                 MultiIndexBlock& indexer,
                                 const BSONObj& spec,
                                 MultiIndexBlock::OnInitFn onInit) {
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto indexBuildInfo = IndexBuildInfo(
        spec, *storageEngine, collection->ns().dbName(), VersionContext::getDecoration(opCtx));
    return indexer
        .init(opCtx,
              collection,
              {indexBuildInfo},
              onInit,
              MultiIndexBlock::InitMode::SteadyState,
              boost::none,
              /*generateTableWrites=*/true)
        .getStatus();
}
}  // namespace mongo
