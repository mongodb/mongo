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


#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_build_block.h"

#include <vector>

#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {

class IndexCatalog;

IndexBuildBlock::IndexBuildBlock(const NamespaceString& nss,
                                 const BSONObj& spec,
                                 IndexBuildMethod method,
                                 boost::optional<UUID> indexBuildUUID)
    : _nss(nss),
      _spec(spec.getOwned()),
      _method(method),
      _buildUUID(indexBuildUUID),
      _pooledBuilder(
          gOperationMemoryPoolBlockInitialSizeKB.loadRelaxed() * static_cast<size_t>(1024),
          SharedBufferFragmentBuilder::DoubleGrowStrategy(
              gOperationMemoryPoolBlockMaxSizeKB.loadRelaxed() * static_cast<size_t>(1024))) {}

void IndexBuildBlock::keepTemporaryTables() {
    if (_indexBuildInterceptor) {
        _indexBuildInterceptor->keepTemporaryTables();
    }
}

void IndexBuildBlock::_completeInit(OperationContext* opCtx, Collection* collection) {
    // Register this index with the CollectionQueryInfo to regenerate the cache. This way, updates
    // occurring while an index is being build in the background will be aware of whether or not
    // they need to modify any indexes.
    auto desc = getEntry(opCtx, collection)->descriptor();
    CollectionQueryInfo::get(collection).rebuildIndexData(opCtx, collection);
    CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
        .registerIndex(desc->indexName(),
                       desc->keyPattern(),
                       IndexFeatures::make(desc, collection->ns().isOnInternalDb()));
}

Status IndexBuildBlock::initForResume(OperationContext* opCtx,
                                      Collection* collection,
                                      const IndexStateInfo& stateInfo,
                                      IndexBuildPhaseEnum phase) {

    _indexName = _spec.getStringField("name").toString();
    auto descriptor = collection->getIndexCatalog()->findIndexByName(
        opCtx,
        _indexName,
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    auto indexCatalogEntry = descriptor->getEntry();

    uassert(4945000,
            "Index catalog entry not found while attempting to resume index build",
            indexCatalogEntry);
    uassert(
        4945001, "Cannot resume a non-hybrid index build", _method == IndexBuildMethod::kHybrid);

    if (phase == IndexBuildPhaseEnum::kBulkLoad) {
        // A bulk cursor can only be opened on a fresh table, so we drop the table that was created
        // before shutdown and recreate it.
        auto status = DurableCatalog::get(opCtx)->dropAndRecreateIndexIdentForResume(
            opCtx,
            collection->ns(),
            collection->getCollectionOptions(),
            descriptor,
            indexCatalogEntry->getIdent());
        if (!status.isOK())
            return status;
    }

    _indexBuildInterceptor =
        std::make_unique<IndexBuildInterceptor>(opCtx,
                                                indexCatalogEntry,
                                                stateInfo.getSideWritesTable(),
                                                stateInfo.getDuplicateKeyTrackerTable(),
                                                stateInfo.getSkippedRecordTrackerTable());
    indexCatalogEntry->setIndexBuildInterceptor(_indexBuildInterceptor.get());

    _completeInit(opCtx, collection);

    return Status::OK();
}

Status IndexBuildBlock::init(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // need this first for names, etc...
    BSONObj keyPattern = _spec.getObjectField("key");
    auto descriptor =
        std::make_unique<IndexDescriptor>(IndexNames::findPluginName(keyPattern), _spec);

    _indexName = descriptor->indexName();

    // Since the index build block is being initialized, the index build for _indexName is
    // beginning. Accordingly, emit an audit event indicating this.
    audit::logCreateIndex(opCtx->getClient(),
                          &_spec,
                          _indexName,
                          collection->ns(),
                          "IndexBuildStarted",
                          ErrorCodes::OK);

    bool isBackgroundIndex = _method == IndexBuildMethod::kHybrid;
    bool isBackgroundSecondaryBuild = false;
    if (auto replCoord = repl::ReplicationCoordinator::get(opCtx)) {
        isBackgroundSecondaryBuild =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet &&
            !replCoord->getMemberState().primary() && isBackgroundIndex;
    }

    // Setup on-disk structures.
    Status status = collection->prepareForIndexBuild(
        opCtx, descriptor.get(), _buildUUID, isBackgroundSecondaryBuild);
    if (!status.isOK())
        return status;

    auto indexCatalogEntry = collection->getIndexCatalog()->createIndexEntry(
        opCtx, collection, std::move(descriptor), CreateIndexEntryFlags::kNone);

    if (_method == IndexBuildMethod::kHybrid) {
        _indexBuildInterceptor = std::make_unique<IndexBuildInterceptor>(opCtx, indexCatalogEntry);
        indexCatalogEntry->setIndexBuildInterceptor(_indexBuildInterceptor.get());
    }

    if (isBackgroundIndex) {
        opCtx->recoveryUnit()->onCommit(
            [entry = indexCatalogEntry, coll = collection](boost::optional<Timestamp> commitTime) {
                // This will prevent the unfinished index from being visible on index iterators.
                if (commitTime) {
                    entry->setMinimumVisibleSnapshot(commitTime.value());
                }
            });
    }

    _completeInit(opCtx, collection);

    return Status::OK();
}

IndexBuildBlock::~IndexBuildBlock() {
    // Don't need to call fail() here, as rollback will clean everything up for us.
}

void IndexBuildBlock::fail(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // Audit that the index build is being aborted.
    audit::logCreateIndex(opCtx->getClient(),
                          &_spec,
                          _indexName,
                          collection->ns(),
                          "IndexBuildAborted",
                          ErrorCodes::IndexBuildAborted);

    auto indexCatalogEntry = getEntry(opCtx, collection);
    if (indexCatalogEntry) {
        invariant(collection->getIndexCatalog()
                      ->dropIndexEntry(opCtx, collection, indexCatalogEntry)
                      .isOK());
        if (_indexBuildInterceptor) {
            indexCatalogEntry->setIndexBuildInterceptor(nullptr);
        }
    } else {
        collection->getIndexCatalog()->deleteIndexFromDisk(opCtx, collection, _indexName);
    }
}

void IndexBuildBlock::success(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    CollectionCatalog::get(opCtx)->invariantHasExclusiveAccessToCollection(opCtx, collection->ns());

    if (_indexBuildInterceptor) {
        // Skipped records are only checked when we complete an index build as primary.
        const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const auto skippedRecordsTracker = _indexBuildInterceptor->getSkippedRecordTracker();
        if (skippedRecordsTracker && replCoord->canAcceptWritesFor(opCtx, collection->ns())) {
            invariant(skippedRecordsTracker->areAllRecordsApplied(opCtx));
        }

        // An index build should never be completed with writes remaining in the interceptor.
        _indexBuildInterceptor->invariantAllWritesApplied(opCtx);
    }

    auto indexCatalogEntry = getEntry(opCtx, collection);
    collection->indexBuildSuccess(opCtx, indexCatalogEntry);
    auto svcCtx = opCtx->getClient()->getServiceContext();

    // Before committing the index build, optimistically audit that the index build has succeeded.
    audit::logCreateIndex(opCtx->getClient(),
                          &_spec,
                          _indexName,
                          collection->ns(),
                          "IndexBuildSucceeded",
                          ErrorCodes::OK);

    opCtx->recoveryUnit()->onCommit(
        [svcCtx,
         indexName = _indexName,
         spec = _spec,
         entry = indexCatalogEntry,
         coll = collection,
         buildUUID = _buildUUID](boost::optional<Timestamp> commitTime) {
            // Note: this runs after the WUOW commits but before we release our X lock on the
            // collection. This means that any snapshot created after this must include the full
            // index, and no one can try to read this index before we set the visibility.
            LOGV2(20345,
                  "Index build: done building index {indexName} on ns {nss}",
                  "Index build: done building",
                  "buildUUID"_attr = buildUUID,
                  "collectionUUID"_attr = coll->uuid(),
                  "namespace"_attr = coll->ns(),
                  "index"_attr = indexName,
                  "ident"_attr = entry->getIdent(),
                  "collectionIdent"_attr = coll->getSharedIdent()->getIdent(),
                  "commitTimestamp"_attr = commitTime);

            if (commitTime) {
                entry->setMinimumVisibleSnapshot(commitTime.value());
            }

            // Add the index to the TTLCollectionCache upon successfully committing the index build.
            // TTL indexes are not compatible with capped collections.
            // Note that TTL deletion is supported on capped clustered collections via bounded
            // collection scan, which does not use an index.
            if (spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName) && !coll->isCapped()) {
                TTLCollectionCache::get(svcCtx).registerTTLInfo(
                    coll->uuid(),
                    TTLCollectionCache::Info{
                        indexName, spec[IndexDescriptor::kExpireAfterSecondsFieldName].isNaN()});
            }
        });
}

const IndexCatalogEntry* IndexBuildBlock::getEntry(OperationContext* opCtx,
                                                   const CollectionPtr& collection) const {
    auto descriptor = collection->getIndexCatalog()->findIndexByName(
        opCtx,
        _indexName,
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    return descriptor->getEntry();
}

IndexCatalogEntry* IndexBuildBlock::getEntry(OperationContext* opCtx, Collection* collection) {
    auto descriptor = collection->getIndexCatalog()->findIndexByName(
        opCtx,
        _indexName,
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    return descriptor->getEntry();
}

}  // namespace mongo
