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


#include "mongo/db/index_builds/index_build_block.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo {

namespace {

bool isBackgroundBuilding(IndexBuildMethodEnum method) {
    return method == IndexBuildMethodEnum::kHybrid ||
        method == IndexBuildMethodEnum::kPrimaryDriven;
}

}  // namespace


IndexBuildBlock::IndexBuildBlock(const NamespaceString& nss,
                                 const BSONObj& spec,
                                 IndexBuildMethodEnum method,
                                 boost::optional<UUID> indexBuildUUID)
    : _nss(nss), _spec(spec.getOwned()), _method(method), _buildUUID(indexBuildUUID) {}

void IndexBuildBlock::keepTemporaryTables() {
    if (_indexBuildInterceptor) {
        _indexBuildInterceptor->keepTemporaryTables();
    }
}

void IndexBuildBlock::_completeInit(OperationContext* opCtx, Collection* collection) {
    // Register this index with the CollectionQueryInfo to regenerate the cache. This way, updates
    // occurring while an index is being build in the background will be aware of whether or not
    // they need to modify any indexes.
    // TODO(SERVER-103400): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
    auto desc = getEntry(opCtx, CollectionPtr::CollectionPtr_UNSAFE(collection))->descriptor();
    CollectionQueryInfo::get(collection).rebuildIndexData(opCtx, collection);
    CollectionIndexUsageTrackerDecoration::write(collection).unregisterIndex(desc->indexName());
    CollectionIndexUsageTrackerDecoration::write(collection)
        .registerIndex(desc->indexName(),
                       desc->keyPattern(),
                       IndexFeatures::make(desc, collection->ns().isOnInternalDb()));
}

Status IndexBuildBlock::initForResume(OperationContext* opCtx,
                                      Collection* collection,
                                      const IndexBuildInfo& indexBuildInfo,
                                      IndexBuildPhaseEnum phase) {
    _indexBuildInfo = indexBuildInfo;
    auto writableEntry = collection->getIndexCatalog()->getWritableEntryByName(
        opCtx,
        getIndexName(),
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    uassert(4945000,
            "Index catalog entry not found while attempting to resume index build",
            writableEntry);
    uassert(4945001,
            "Cannot resume a non-hybrid index build",
            _method == IndexBuildMethodEnum::kHybrid);

    if (phase == IndexBuildPhaseEnum::kBulkLoad) {
        // A bulk cursor can only be opened on a fresh table, so we drop the table that was created
        // before shutdown and recreate it.
        auto collectionOptions = collection->getCollectionOptions();
        auto status = durable_catalog::dropAndRecreateIndexIdentForResume(
            opCtx,
            collection->ns(),
            collection->getCollectionOptions(),
            writableEntry->descriptor()->toIndexConfig(),
            writableEntry->getIdent());
        if (!status.isOK())
            return status;
    }

    _indexBuildInterceptor = std::make_unique<IndexBuildInterceptor>(
        opCtx, writableEntry, indexBuildInfo, /*resume=*/true);
    writableEntry->setIndexBuildInterceptor(_indexBuildInterceptor.get());

    _completeInit(opCtx, collection);

    return Status::OK();
}

Status IndexBuildBlock::init(OperationContext* opCtx,
                             Collection* collection,
                             const IndexBuildInfo& indexBuildInfo,
                             bool forRecovery) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // need this first for names, etc...
    BSONObj keyPattern = _spec.getObjectField("key");
    auto descriptor = IndexDescriptor(IndexNames::findPluginName(keyPattern), _spec);

    _indexBuildInfo = indexBuildInfo;

    // Since the index build block is being initialized, the index build is beginning. Accordingly,
    // emit an audit event indicating this.
    audit::logCreateIndex(opCtx->getClient(),
                          &_spec,
                          getIndexName(),
                          collection->ns(),
                          "IndexBuildStarted",
                          ErrorCodes::OK);

    if (!forRecovery) {
        // Setup on-disk structures. We skip this during startup recovery for unfinished indexes as
        // everything is already in-place.
        Status status = collection->prepareForIndexBuild(
            opCtx, &descriptor, _indexBuildInfo->indexIdent, _buildUUID);
        if (!status.isOK())
            return status;
    }

    if (isBackgroundBuilding(_method)) {
        auto indexCatalog = collection->getIndexCatalog();
        auto indexCatalogEntry = indexCatalog->getWritableEntryByName(
            opCtx, getIndexName(), IndexCatalog::InclusionPolicy::kUnfinished);
        _indexBuildInterceptor = std::make_unique<IndexBuildInterceptor>(
            opCtx, indexCatalogEntry, *_indexBuildInfo, /*resume=*/false);
        indexCatalogEntry->setIndexBuildInterceptor(_indexBuildInterceptor.get());
    }

    _completeInit(opCtx, collection);

    return Status::OK();
}

IndexBuildBlock::~IndexBuildBlock() {
    // Don't need to call fail() here, as rollback will clean everything up for us.
}

void IndexBuildBlock::fail(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // Audit that the index build is being aborted.
    audit::logCreateIndex(opCtx->getClient(),
                          &_spec,
                          getIndexName(),
                          collection->ns(),
                          "IndexBuildAborted",
                          ErrorCodes::IndexBuildAborted);

    if (auto indexCatalogEntry = getWritableEntry(opCtx, collection)) {
        if (_indexBuildInterceptor) {
            indexCatalogEntry->setIndexBuildInterceptor(nullptr);
        }
        invariant(
            collection->getIndexCatalog()->dropIndexEntry(opCtx, collection, indexCatalogEntry));
    } else {
        collection->getIndexCatalog()->deleteIndexFromDisk(opCtx, collection, getIndexName());
    }
}

void IndexBuildBlock::success(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    CollectionCatalog::get(opCtx)->invariantHasExclusiveAccessToCollection(opCtx, collection->ns());

    if (_indexBuildInterceptor) {
        // Skipped records are only checked when we complete an index build as primary.
        const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const auto skippedRecordsTracker = _indexBuildInterceptor->getSkippedRecordTracker();
        if (skippedRecordsTracker && replCoord->canAcceptWritesFor(opCtx, collection->ns())) {
            invariant(skippedRecordsTracker->areAllRecordsApplied(opCtx));
        }

        // TODO(SERVER-107055): Remove the following condition after standby is able to receive and
        // apply container ops.
        if (_method != IndexBuildMethodEnum::kPrimaryDriven ||
            replCoord->canAcceptWritesFor(opCtx, collection->ns())) {
            // An index build should never be completed with writes remaining in the interceptor.
            _indexBuildInterceptor->invariantAllWritesApplied(opCtx);
        }
    }

    auto indexCatalogEntry = getWritableEntry(opCtx, collection);
    collection->indexBuildSuccess(opCtx, indexCatalogEntry);
    auto svcCtx = opCtx->getClient()->getServiceContext();

    // Before committing the index build, optimistically audit that the index build has succeeded.
    audit::logCreateIndex(opCtx->getClient(),
                          &_spec,
                          getIndexName(),
                          collection->ns(),
                          "IndexBuildSucceeded",
                          ErrorCodes::OK);

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [svcCtx,
         indexName = getIndexName(),
         spec = _spec,
         ident = indexCatalogEntry->getIdent(),
         coll = collection,
         buildUUID = _buildUUID](OperationContext*, boost::optional<Timestamp> commitTime) {
            // Note: this runs after the WUOW commits but before we release our X lock on the
            // collection. This means that any snapshot created after this must include the full
            // index, and no one can try to read this index before we set the visibility.
            LOGV2(20345,
                  "Index build: done building",
                  "buildUUID"_attr = buildUUID,
                  "collectionUUID"_attr = coll->uuid(),
                  logAttrs(coll->ns()),
                  "index"_attr = indexName,
                  "ident"_attr = ident,
                  "collectionIdent"_attr = coll->getSharedIdent()->getIdent(),
                  "commitTimestamp"_attr = commitTime);

            // Add the index to the TTLCollectionCache upon successfully committing the index build.
            // Note that TTL deletion is supported on capped clustered collections via bounded
            // collection scan, which does not use an index.
            if (spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
                auto swType = index_key_validate::validateExpireAfterSeconds(
                    spec[IndexDescriptor::kExpireAfterSecondsFieldName],
                    index_key_validate::ValidateExpireAfterSecondsMode::kSecondaryTTLIndex);
                TTLCollectionCache::get(svcCtx).registerTTLInfo(
                    coll->uuid(),
                    TTLCollectionCache::Info{
                        indexName, index_key_validate::extractExpireAfterSecondsType(swType)});
            }
        });
}

const IndexCatalogEntry* IndexBuildBlock::getEntry(OperationContext* opCtx,
                                                   const CollectionPtr& collection) const {
    auto descriptor = collection->getIndexCatalog()->findIndexByName(
        opCtx,
        getIndexName(),
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    return descriptor->getEntry();
}

IndexCatalogEntry* IndexBuildBlock::getWritableEntry(OperationContext* opCtx,
                                                     Collection* collection) {
    return collection->getIndexCatalog()->getWritableEntryByName(
        opCtx,
        getIndexName(),
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
}

Status IndexBuildBlock::buildEmptyIndex(OperationContext* opCtx,
                                        Collection* collection,
                                        const IndexBuildInfo& indexBuildInfo) {
    IndexBuildBlock indexBuildBlock(
        collection->ns(), indexBuildInfo.spec, IndexBuildMethodEnum::kForeground, boost::none);
    if (auto status =
            indexBuildBlock.init(opCtx, collection, indexBuildInfo, /*forRecovery=*/false);
        !status.isOK()) {
        return status;
    }

    // sanity checks, etc...
    IndexCatalogEntry* entry = indexBuildBlock.getWritableEntry(opCtx, collection);
    invariant(entry);
    IndexDescriptor* descriptor = entry->descriptor();
    invariant(descriptor);

    if (auto status = entry->accessMethod()->initializeAsEmpty(); !status.isOK())
        return status;
    indexBuildBlock.success(opCtx, collection);

    // sanity check
    invariant(collection->isIndexReady(descriptor->indexName()));

    return Status::OK();
}

}  // namespace mongo
