// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_test_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"

#include <string_view>

namespace mongo {
Status createIndex(OperationContext* opCtx, std::string_view ns, const BSONObj& keys, bool unique) {
    BSONObjBuilder specBuilder;
    specBuilder.append("name", DBClientBase::genIndexName(keys));
    specBuilder.append("key", keys);
    specBuilder.append("v", 2);
    if (unique) {
        specBuilder.appendBool("unique", true);
    }
    return createIndexFromSpec(opCtx, ns, specBuilder.done());
}

Status createIndexFromSpec(OperationContext* opCtx, std::string_view ns, const BSONObj& spec) {
    return createIndexFromSpec(opCtx, nullptr, ns, spec);
}

Status createIndexFromSpec(OperationContext* opCtx,
                           VectorClockMutable* clock,
                           std::string_view ns,
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
        WriteUnitOfWork wunit(opCtx);
        auto acq = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        if (!acq.exists()) {
            auto db = DatabaseHolder::get(opCtx)->openDb(opCtx, nss.dbName());
            invariant(db);
            auto coll =
                db->createCollection(opCtx, NamespaceString::createNamespaceString_forTest(ns));
            invariant(coll);
        }
        wunit.commit();
    }

    MultiIndexBlock indexer;
    ScopeGuard abortOnExit([&] {
        auto acq = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter collection(opCtx, &acq);
        indexer.abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
    });

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    IndexBuildInfo indexBuildInfo(spec, *storageEngine, nss.dbName());
    {
        auto acq = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter collection(opCtx, &acq);
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
                                boost::none)
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

    auto acq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
    CollectionWriter collection(opCtx, &acq);
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
    auto indexBuildInfo = IndexBuildInfo(spec, *storageEngine, collection->ns().dbName());
    return indexer
        .init(opCtx,
              collection,
              {indexBuildInfo},
              onInit,
              MultiIndexBlock::InitMode::SteadyState,
              boost::none)
        .getStatus();
}
}  // namespace mongo
