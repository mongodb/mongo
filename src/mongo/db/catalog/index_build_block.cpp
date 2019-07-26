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

#include "mongo/db/catalog/index_build_block.h"

#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

class IndexCatalog;

IndexBuildBlock::IndexBuildBlock(IndexCatalog* indexCatalog,
                                 const NamespaceString& nss,
                                 const BSONObj& spec,
                                 IndexBuildMethod method)
    : _indexCatalog(indexCatalog),
      _nss(nss),
      _spec(spec.getOwned()),
      _method(method),
      _indexCatalogEntry(nullptr) {}

void IndexBuildBlock::deleteTemporaryTables(OperationContext* opCtx) {
    if (_indexBuildInterceptor) {
        _indexBuildInterceptor->deleteTemporaryTables(opCtx);
    }
}

Status IndexBuildBlock::init(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // need this first for names, etc...
    BSONObj keyPattern = _spec.getObjectField("key");
    auto descriptor = std::make_unique<IndexDescriptor>(
        collection, IndexNames::findPluginName(keyPattern), _spec);

    _indexName = descriptor->indexName();

    bool isBackgroundIndex =
        _method == IndexBuildMethod::kHybrid || _method == IndexBuildMethod::kBackground;
    bool isBackgroundSecondaryBuild = false;
    if (auto replCoord = repl::ReplicationCoordinator::get(opCtx)) {
        isBackgroundSecondaryBuild =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet &&
            !replCoord->getMemberState().primary() && isBackgroundIndex;
    }

    // Setup on-disk structures.
    const auto protocol = IndexBuildProtocol::kSinglePhase;
    Status status = DurableCatalog::get(opCtx)->prepareForIndexBuild(
        opCtx, _nss, descriptor.get(), protocol, isBackgroundSecondaryBuild);
    if (!status.isOK())
        return status;

    const bool initFromDisk = false;
    const bool isReadyIndex = false;
    _indexCatalogEntry =
        _indexCatalog->createIndexEntry(opCtx, std::move(descriptor), initFromDisk, isReadyIndex);

    if (_method == IndexBuildMethod::kHybrid) {
        _indexBuildInterceptor = std::make_unique<IndexBuildInterceptor>(opCtx, _indexCatalogEntry);
        _indexCatalogEntry->setIndexBuildInterceptor(_indexBuildInterceptor.get());

        if (IndexBuildProtocol::kTwoPhase == protocol) {
            const auto sideWritesIdent = _indexBuildInterceptor->getSideWritesTableIdent();
            // Only unique indexes have a constraint violations table.
            const auto constraintsIdent = (_indexCatalogEntry->descriptor()->unique())
                ? boost::optional<std::string>(
                      _indexBuildInterceptor->getConstraintViolationsTableIdent())
                : boost::none;

            DurableCatalog::get(opCtx)->setIndexBuildScanning(
                opCtx,
                _nss,
                _indexCatalogEntry->descriptor()->indexName(),
                sideWritesIdent,
                constraintsIdent);
        }
    }

    if (isBackgroundIndex) {
        opCtx->recoveryUnit()->onCommit(
            [entry = _indexCatalogEntry, coll = collection](boost::optional<Timestamp> commitTime) {
                // This will prevent the unfinished index from being visible on index iterators.
                if (commitTime) {
                    entry->setMinimumVisibleSnapshot(commitTime.get());
                    coll->setMinimumVisibleSnapshot(commitTime.get());
                }
            });
    }

    // Register this index with the CollectionInfoCache to regenerate the cache. This way, updates
    // occurring while an index is being build in the background will be aware of whether or not
    // they need to modify any indexes.
    collection->infoCache()->addedIndex(opCtx, _indexCatalogEntry->descriptor());

    return Status::OK();
}

IndexBuildBlock::~IndexBuildBlock() {
    // Don't need to call fail() here, as rollback will clean everything up for us.
}

void IndexBuildBlock::fail(OperationContext* opCtx, const Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    fassert(17204, collection->ok());  // defensive

    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_X));

    if (_indexCatalogEntry) {
        invariant(_indexCatalog->dropIndexEntry(opCtx, _indexCatalogEntry).isOK());
        if (_indexBuildInterceptor) {
            _indexCatalogEntry->setIndexBuildInterceptor(nullptr);
        }
    } else {
        _indexCatalog->deleteIndexFromDisk(opCtx, _indexName);
    }
}

void IndexBuildBlock::success(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    fassert(17207, collection->ok());
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_X));

    if (_indexBuildInterceptor) {
        // An index build should never be completed with writes remaining in the interceptor.
        invariant(_indexBuildInterceptor->areAllWritesApplied(opCtx));

        // An index build should never be completed without resolving all key constraints.
        invariant(_indexBuildInterceptor->areAllConstraintsChecked(opCtx));
    }

    log() << "index build: done building index " << _indexName << " on ns " << _nss;

    collection->indexBuildSuccess(opCtx, _indexCatalogEntry);

    opCtx->recoveryUnit()->onCommit([opCtx, entry = _indexCatalogEntry, coll = collection](
                                        boost::optional<Timestamp> commitTime) {
        // Note: this runs after the WUOW commits but before we release our X lock on the
        // collection. This means that any snapshot created after this must include the full
        // index, and no one can try to read this index before we set the visibility.
        if (!commitTime) {
            // The end of background index builds on secondaries does not get a commit
            // timestamp. We use the cluster time since it's guaranteed to be greater than the
            // time of the index build. It is possible the cluster time could be in the future,
            // and we will need to do another write to reach the minimum visible snapshot.
            commitTime = LogicalClock::getClusterTimeForReplicaSet(opCtx).asTimestamp();
        }
        entry->setMinimumVisibleSnapshot(commitTime.get());
        // We must also set the minimum visible snapshot on the collection like during init().
        // This prevents reads in the past from reading inconsistent metadata. We should be
        // able to remove this when the catalog is versioned.
        coll->setMinimumVisibleSnapshot(commitTime.get());
    });
}

}  // namespace mongo
