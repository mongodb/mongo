// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/index_builds/index_build_block.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_builds/primary_driven/enabled.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/index_names.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
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

void IndexBuildBlock::_completeInit(OperationContext* opCtx, Collection* collection) {
    // Register this index with the CollectionQueryInfo to regenerate the cache. This way, updates
    // occurring while an index is being build in the background will be aware of whether or not
    // they need to modify any indexes.
    auto desc = getEntry(opCtx, collection)->descriptor();
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
                                      IndexTableResumeBehavior behavior) {
    _indexBuildInfo = indexBuildInfo;
    auto writableEntry = collection->getIndexCatalog()->getWritableEntryByName(
        opCtx,
        getIndexName(),
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);

    uassert(4945000,
            "Index catalog entry not found while attempting to resume index build",
            writableEntry);
    // TODO (SERVER-109664): Remove kPrimaryDriven method check.
    uassert(4945001,
            "Cannot resume a non-hybrid index build",
            _method == IndexBuildMethodEnum::kHybrid ||
                _method == IndexBuildMethodEnum::kPrimaryDriven);

    if (behavior == IndexTableResumeBehavior::recreate) {
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

    _indexBuildInterceptor =
        std::make_shared<IndexBuildInterceptor>(opCtx,
                                                indexBuildInfo,
                                                LazyRecordStore::CreateMode::openExisting,
                                                writableEntry->descriptor()->unique());
    writableEntry->setIndexBuildInterceptor(_indexBuildInterceptor);

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
        // Primary-driven index builds use replicated tables rather than temporary local tables, so
        // they need to be created at a consistent timestamp on all nodes. Currently this is done by
        // creating them eagerly rather than as needed.
        auto isPrimaryDrivenIndexBuild = index_builds::primary_driven::enabled(opCtx);
        auto mode = isPrimaryDrivenIndexBuild ? LazyRecordStore::CreateMode::immediate
                                              : LazyRecordStore::CreateMode::deferred;

        auto indexCatalog = collection->getIndexCatalog();
        auto indexCatalogEntry = indexCatalog->getWritableEntryByName(
            opCtx, getIndexName(), IndexCatalog::InclusionPolicy::kUnfinished);
        _indexBuildInterceptor = std::make_shared<IndexBuildInterceptor>(
            opCtx, *_indexBuildInfo, mode, indexCatalogEntry->descriptor()->unique());
        indexCatalogEntry->setIndexBuildInterceptor(_indexBuildInterceptor);
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
        if (replCoord->canAcceptWritesFor(opCtx, collection->ns())) {
            invariant(!_indexBuildInterceptor->hasAnySkippedRecords(opCtx));
        }

        // An index build should never be completed with writes remaining in the interceptor.
        _indexBuildInterceptor->invariantAllWritesApplied(opCtx);
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
    return collection->getIndexCatalog()->findIndexByName(
        opCtx,
        getIndexName(),
        IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
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
    if (auto status = indexBuildBlock.init(opCtx,
                                           collection,
                                           indexBuildInfo,
                                           /*forRecovery=*/false);
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
    // We can rebuild the path arrayness information once this index is ready.
    if (feature_flags::gFeatureFlagPathArrayness.isEnabled()) {
        CollectionQueryInfo::get(collection).rebuildPathArrayness(opCtx, collection);
    }
    join_ordering::bumpCollectionVersionForDDL(collection);

    return Status::OK();
}

}  // namespace mongo
