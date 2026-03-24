/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/index_builds/primary_driven/util.h"

#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/db/audit.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/ttl/ttl_collection_cache.h"

namespace mongo::index_builds::primary_driven {
namespace {

auto _registry = ServiceContext::declareDecoration<Registry>();

std::vector<boost::optional<BSONObj>> multikeyPathsToObjs(
    const std::vector<IndexBuildInfo>& indexes,
    const std::vector<boost::optional<MultikeyPaths>>& multikeyPaths) {
    std::vector<boost::optional<BSONObj>> multikeyObjs;
    multikeyObjs.reserve(multikeyPaths.size());
    for (size_t i = 0; i < multikeyPaths.size(); ++i) {
        if (multikeyPaths[i]) {
            multikeyObjs.push_back(
                !multikeyPaths[i]->empty()
                    ? multikey_paths::serialize(indexes[i].spec.getObjectField("key"),
                                                *multikeyPaths[i])
                    : BSONObj{});
        } else {
            multikeyObjs.push_back(boost::none);
        }
    }
    return multikeyObjs;
}

}  // namespace

Registry& registry(ServiceContext* svcCtx) {
    return _registry(svcCtx);
}

Status start(OperationContext* opCtx,
             DatabaseName dbName,
             const UUID& collectionUUID,
             const UUID& buildUUID,
             std::vector<IndexBuildInfo> indexes) {
    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, {dbName, collectionUUID}, AcquisitionPrerequisites::OperationType::kWrite),
        LockMode::MODE_X);
    CollectionWriter writer{opCtx, &coll};
    WriteUnitOfWork wuow{opCtx};
    auto writableColl = writer.getWritableCollection(opCtx);

    for (auto&& index : indexes) {
        auto spec = writer->getIndexCatalog()->prepareSpecForCreate(
            opCtx, writer.get(), index.spec, boost::none);
        if (!spec.isOK()) {
            return spec.getStatus();
        }

        auto descriptor = IndexDescriptor{
            IndexNames::findPluginName(spec.getValue().getObjectField("key")), spec.getValue()};
        auto status =
            writableColl->prepareForIndexBuild(opCtx, &descriptor, index.indexIdent, buildUUID);
        if (!status.isOK()) {
            return status;
        }

        IndexBuildInterceptor interceptor{
            opCtx, index, LazyRecordStore::CreateMode::immediate, descriptor.unique(), false};
        interceptor.keepTemporaryTables(opCtx);

        CollectionQueryInfo::get(writableColl).rebuildIndexData(opCtx, writableColl);
        CollectionIndexUsageTrackerDecoration::write(writableColl)
            .unregisterIndex(descriptor.indexName());
        CollectionIndexUsageTrackerDecoration::write(writableColl)
            .registerIndex(descriptor.indexName(),
                           descriptor.keyPattern(),
                           IndexFeatures::make(&descriptor, coll.nss().isOnInternalDb()));

        audit::logCreateIndex(opCtx->getClient(),
                              &index.spec,
                              index.getIndexName(),
                              coll.nss(),
                              "IndexBuildStarted",
                              ErrorCodes::OK);
    }

    opCtx->getServiceContext()->getOpObserver()->onStartIndexBuild(
        opCtx, coll.nss(), collectionUUID, buildUUID, indexes, /*fromMigrate=*/false);
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [dbName = std::move(dbName), collectionUUID, buildUUID, indexes = std::move(indexes)](
            OperationContext* opCtx, boost::optional<Timestamp>) {
            _registry(opCtx->getServiceContext())
                .add(buildUUID, std::move(dbName), collectionUUID, std::move(indexes));
        });

    wuow.commit();
    return Status::OK();
}

Status commit(OperationContext* opCtx,
              DatabaseName dbName,
              const UUID& collectionUUID,
              const UUID& buildUUID,
              const std::vector<IndexBuildInfo>& indexes,
              const std::vector<boost::optional<MultikeyPaths>>& multikey) {
    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, {dbName, collectionUUID}, AcquisitionPrerequisites::OperationType::kWrite),
        LockMode::MODE_X);
    CollectionWriter writer{opCtx, &coll};
    WriteUnitOfWork wuow{opCtx};
    auto writableColl = writer.getWritableCollection(opCtx);

    for (size_t i = 0; i < indexes.size(); ++i) {
        auto&& index = indexes[i];

        auto entry = writableColl->getIndexCatalog()->getWritableEntryByName(
            opCtx, index.getIndexName(), IndexCatalog::InclusionPolicy::kUnfinished);

        {
            IndexBuildInterceptor interceptor{opCtx,
                                              index,
                                              LazyRecordStore::CreateMode::openExisting,
                                              entry->descriptor()->unique(),
                                              false};
        }

        writableColl->indexBuildSuccess(opCtx, entry);
        if (multikey[i]) {
            entry->setMultikey(opCtx, writer.get(), {}, *multikey[i]);
        }

        auto& collectionQueryInfo = CollectionQueryInfo::get(writableColl);
        collectionQueryInfo.clearQueryCache(opCtx, writer.get());
        if (mongo::feature_flags::gFeatureFlagPathArrayness.isEnabled()) {
            collectionQueryInfo.rebuildPathArrayness(opCtx, writableColl);
        }

        audit::logCreateIndex(opCtx->getClient(),
                              &index.spec,
                              index.getIndexName(),
                              coll.nss(),
                              "IndexBuildSucceeded",
                              ErrorCodes::OK);

        if (index.spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
            shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                [collectionUUID, indexName = std::string{index.getIndexName()}, spec = index.spec](
                    OperationContext* opCtx, boost::optional<Timestamp>) {
                    TTLCollectionCache::get(opCtx->getServiceContext())
                        .registerTTLInfo(
                            collectionUUID,
                            TTLCollectionCache::Info{
                                indexName,
                                index_key_validate::extractExpireAfterSecondsType(
                                    index_key_validate::validateExpireAfterSeconds(
                                        spec[IndexDescriptor::kExpireAfterSecondsFieldName],
                                        index_key_validate::ValidateExpireAfterSecondsMode::
                                            kSecondaryTTLIndex))});
                });
        }
    }

    opCtx->getServiceContext()->getOpObserver()->onCommitIndexBuild(
        opCtx,
        coll.nss(),
        collectionUUID,
        buildUUID,
        indexes,
        multikeyPathsToObjs(indexes, multikey),
        /*fromMigrate=*/false);
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [buildUUID](OperationContext* opCtx, boost::optional<Timestamp>) {
            _registry(opCtx->getServiceContext()).remove(buildUUID);
        });

    wuow.commit();
    return Status::OK();
}

Status abort(OperationContext* opCtx,
             DatabaseName dbName,
             const UUID& collectionUUID,
             const UUID& buildUUID,
             const std::vector<IndexBuildInfo>& indexes,
             const Status& cause) {
    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, {dbName, collectionUUID}, AcquisitionPrerequisites::OperationType::kWrite),
        LockMode::MODE_X);
    CollectionWriter writer{opCtx, &coll};
    WriteUnitOfWork wuow{opCtx};
    auto writableColl = writer.getWritableCollection(opCtx);

    for (auto&& index : indexes) {
        auto entry = writableColl->getIndexCatalog()->getWritableEntryByName(
            opCtx, index.getIndexName(), IndexCatalog::InclusionPolicy::kUnfinished);

        {
            // TODO (SERVER-121124): Use idents provided in index.
            auto mutableIndex = index;
            mutableIndex.indexIdent = entry->getIdent();
            mutableIndex.setInternalIdents(*opCtx->getServiceContext()->getStorageEngine());
            IndexBuildInterceptor interceptor{opCtx,
                                              mutableIndex,
                                              LazyRecordStore::CreateMode::openExisting,
                                              entry->descriptor()->unique(),
                                              false};
        }

        auto status = writableColl->getIndexCatalog()->dropIndexEntry(opCtx, writableColl, entry);
        if (!status.isOK()) {
            return status;
        }

        audit::logCreateIndex(opCtx->getClient(),
                              &index.spec,
                              index.getIndexName(),
                              coll.nss(),
                              "IndexBuildAborted",
                              ErrorCodes::IndexBuildAborted);
    }

    opCtx->getServiceContext()->getOpObserver()->onAbortIndexBuild(opCtx,
                                                                   coll.nss(),
                                                                   collectionUUID,
                                                                   buildUUID,
                                                                   toIndexSpecs(indexes),
                                                                   cause,
                                                                   /*fromMigrate=*/false);
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [buildUUID](OperationContext* opCtx, boost::optional<Timestamp>) {
            _registry(opCtx->getServiceContext()).remove(buildUUID);
        });

    wuow.commit();
    return Status::OK();
}

}  // namespace mongo::index_builds::primary_driven
