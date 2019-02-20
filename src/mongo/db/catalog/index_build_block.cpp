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

#include "mongo/db/catalog/index_catalog_impl.h"

#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
IndexCatalogImpl::IndexBuildBlock::IndexBuildBlock(IndexCatalogImpl* catalog,
                                                   const NamespaceString& nss,
                                                   const BSONObj& spec,
                                                   IndexBuildMethod method)
    : _catalog(catalog), _ns(nss.ns()), _spec(spec.getOwned()), _method(method), _entry(nullptr) {}

void IndexCatalogImpl::IndexBuildBlock::deleteTemporaryTables(OperationContext* opCtx) {
    if (_indexBuildInterceptor) {
        _indexBuildInterceptor->deleteTemporaryTables(opCtx);
    }
}

Status IndexCatalogImpl::IndexBuildBlock::init(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // need this first for names, etc...
    BSONObj keyPattern = _spec.getObjectField("key");
    auto descriptor = stdx::make_unique<IndexDescriptor>(
        collection, IndexNames::findPluginName(keyPattern), _spec);

    _indexName = descriptor->indexName();
    _indexNamespace = descriptor->indexNamespace();

    bool isBackgroundIndex =
        _method == IndexBuildMethod::kHybrid || _method == IndexBuildMethod::kBackground;
    bool isBackgroundSecondaryBuild = false;
    if (auto replCoord = repl::ReplicationCoordinator::get(opCtx)) {
        isBackgroundSecondaryBuild =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet &&
            replCoord->getMemberState().secondary() && isBackgroundIndex;
    }

    // Setup on-disk structures.
    const auto protocol = IndexBuildProtocol::kTwoPhase;
    Status status = collection->getCatalogEntry()->prepareForIndexBuild(
        opCtx, descriptor.get(), protocol, isBackgroundSecondaryBuild);
    if (!status.isOK())
        return status;

    const bool initFromDisk = false;
    const bool isReadyIndex = false;
    _entry = _catalog->_setupInMemoryStructures(
        opCtx, std::move(descriptor), initFromDisk, isReadyIndex);

    if (_method == IndexBuildMethod::kHybrid) {
        _indexBuildInterceptor = stdx::make_unique<IndexBuildInterceptor>(opCtx, _entry);
        _entry->setIndexBuildInterceptor(_indexBuildInterceptor.get());

        const auto sideWritesIdent = _indexBuildInterceptor->getSideWritesTableIdent();
        // Only unique indexes have a constraint violations table.
        const auto constraintsIdent = (_entry->descriptor()->unique())
            ? boost::optional<std::string>(
                  _indexBuildInterceptor->getConstraintViolationsTableIdent())
            : boost::none;

        if (IndexBuildProtocol::kTwoPhase == protocol) {
            collection->getCatalogEntry()->setIndexBuildScanning(
                opCtx, _entry->descriptor()->indexName(), sideWritesIdent, constraintsIdent);
        }
    }

    if (isBackgroundIndex) {
        opCtx->recoveryUnit()->onCommit(
            [ entry = _entry, coll = collection ](boost::optional<Timestamp> commitTime) {
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
    collection->infoCache()->addedIndex(opCtx, _entry->descriptor());

    return Status::OK();
}

IndexCatalogImpl::IndexBuildBlock::~IndexBuildBlock() {
    // Don't need to call fail() here, as rollback will clean everything up for us.
}

void IndexCatalogImpl::IndexBuildBlock::fail(OperationContext* opCtx,
                                             const Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    fassert(17204, collection->ok());  // defensive

    NamespaceString ns(_indexNamespace);
    invariant(opCtx->lockState()->isDbLockedForMode(ns.db(), MODE_X));

    if (_entry) {
        invariant(_catalog->_dropIndex(opCtx, _entry).isOK());
        if (_indexBuildInterceptor) {
            _entry->setIndexBuildInterceptor(nullptr);
        }
    } else {
        _catalog->_deleteIndexFromDisk(opCtx, _indexName, _indexNamespace);
    }
}

void IndexCatalogImpl::IndexBuildBlock::success(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    fassert(17207, collection->ok());
    NamespaceString ns(_indexNamespace);
    invariant(opCtx->lockState()->isDbLockedForMode(ns.db(), MODE_X));

    if (_indexBuildInterceptor) {
        // An index build should never be completed with writes remaining in the interceptor.
        invariant(_indexBuildInterceptor->areAllWritesApplied(opCtx));

        // An index build should never be completed without resolving all key constraints.
        invariant(_indexBuildInterceptor->areAllConstraintsChecked(opCtx));
    }

    log() << "index build: done building index " << _indexName << " on ns " << _ns;

    collection->indexBuildSuccess(opCtx, _entry);

    opCtx->recoveryUnit()->onCommit(
        [ opCtx, entry = _entry, coll = collection ](boost::optional<Timestamp> commitTime) {
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
