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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index_builder.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_timestamp_helper.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

AtomicWord<unsigned> IndexBuilder::_indexBuildCount;

namespace {

const StringData kIndexesFieldName = "indexes"_sd;
const StringData kCommandName = "createIndexes"_sd;

}  // namespace

IndexBuilder::IndexBuilder(const BSONObj& index,
                           IndexConstraints indexConstraints,
                           ReplicatedWrites replicatedWrites,
                           Timestamp initIndexTs)
    : _index(index.getOwned()),
      _indexConstraints(indexConstraints),
      _replicatedWrites(replicatedWrites),
      _initIndexTs(initIndexTs),
      _name(str::stream() << "repl-index-builder-" << _indexBuildCount.addAndFetch(1)) {}

IndexBuilder::~IndexBuilder() {}

bool IndexBuilder::canBuildInBackground() {
    return MultiIndexBlock::areHybridIndexBuildsEnabled();
}

std::string IndexBuilder::name() const {
    return _name;
}

Status IndexBuilder::buildInForeground(OperationContext* opCtx,
                                       Database* db,
                                       Collection* coll) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns(), MODE_X));

    // Collections should not be implicitly created by the index builder.
    fassert(40409, coll);

    MultiIndexBlock indexer;

    // The 'indexer' can throw, so ensure build cleanup occurs.
    ON_BLOCK_EXIT([&] { indexer.cleanUpAfterBuild(opCtx, coll); });

    return _build(opCtx, coll, indexer);
}

Status IndexBuilder::_build(OperationContext* opCtx,
                            Collection* coll,
                            MultiIndexBlock& indexer) const try {
    auto ns = coll->ns();

    {
        BSONObjBuilder builder;
        builder.append(kCommandName, ns.coll());
        {
            BSONArrayBuilder indexesBuilder;
            indexesBuilder.append(_index);
            builder.append(kIndexesFieldName, indexesBuilder.arr());
        }
        auto opDescObj = builder.obj();

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // Show which index we're building in the curop display.
        auto curOp = CurOp::get(opCtx);
        curOp->setLogicalOp_inlock(LogicalOp::opCommand);
        curOp->setNS_inlock(ns.ns());
        curOp->setOpDescription_inlock(opDescObj);
    }

    // Ignore uniqueness constraint violations when relaxed (on secondaries). Secondaries can
    // complete index builds in the middle of batches, which creates the potential for finding
    // duplicate key violations where there otherwise would be none at consistent states.
    if (_indexConstraints == IndexConstraints::kRelax) {
        indexer.ignoreUniqueConstraint();
    }

    Status status = Status::OK();

    {
        TimestampBlock tsBlock(opCtx, _initIndexTs);
        status = writeConflictRetry(opCtx, "Init index build", ns.ns(), [&] {
            return indexer
                .init(
                    opCtx, coll, _index, MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, coll))
                .getStatus();
        });
    }

    if (status == ErrorCodes::IndexAlreadyExists ||
        (status == ErrorCodes::IndexOptionsConflict &&
         _indexConstraints == IndexConstraints::kRelax)) {
        LOG(1) << "Ignoring indexing error: " << redact(status);

        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }

    {
        // WriteConflict exceptions and statuses are not expected to escape this method.
        status = indexer.insertAllDocumentsInCollection(opCtx, coll);
        if (!status.isOK()) {
            return status;
        }

        status = indexer.checkConstraints(opCtx);
        if (!status.isOK()) {
            return status;
        }
    }

    // Emit startIndexBuild and commitIndexBuild oplog entries if supported by the current
    // FCV.
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto fromMigrate = false;
    auto buildUUID = serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            serverGlobalParams.featureCompatibility.getVersion() ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44
        ? boost::make_optional(UUID::gen())
        : boost::none;

    if (buildUUID) {
        opObserver->onStartIndexBuild(
            opCtx, coll->ns(), coll->uuid(), *buildUUID, {_index}, fromMigrate);
    }

    status = writeConflictRetry(
        opCtx,
        "Commit index build",
        ns.ns(),
        [opCtx, coll, buildUUID, fromMigrate, &spec = _index, &indexer, &ns] {
            WriteUnitOfWork wunit(opCtx);

            auto status = indexer.commit(
                opCtx,
                coll,
                [opCtx, coll, buildUUID, fromMigrate, &ns](const BSONObj& indexSpec) {
                    // If two phase index builds are enabled, the index build will be coordinated
                    // using startIndexBuild and commitIndexBuild oplog entries.
                    if (!IndexBuildsCoordinator::get(opCtx)->supportsTwoPhaseIndexBuild()) {
                        opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                            opCtx, ns, coll->uuid(), indexSpec, fromMigrate);
                    }
                },
                [opCtx, coll, buildUUID, fromMigrate, &spec, &ns] {
                    if (buildUUID) {
                        opCtx->getServiceContext()->getOpObserver()->onCommitIndexBuild(
                            opCtx, coll->ns(), coll->uuid(), *buildUUID, {spec}, fromMigrate);
                    }
                });
            if (!status.isOK()) {
                return status;
            }

            IndexTimestampHelper::setGhostCommitTimestampForCatalogWrite(opCtx, ns);
            wunit.commit();
            return Status::OK();
        });
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace mongo
