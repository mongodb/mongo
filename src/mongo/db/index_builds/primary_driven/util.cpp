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
#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/index_builds/multi_index_block_gen.h"
#include "mongo/db/index_builds/resumable_index_builds_common.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/ttl/ttl_collection_cache.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo::index_builds::primary_driven {
namespace {

auto _registry = ServiceContext::declareDecoration<Registry>();

std::vector<std::string> getIndexBuildIdents(const std::vector<IndexBuildInfo>& indexes,
                                             const boost::optional<std::string>& indexBuildIdent) {
    std::vector<std::string> idents;
    for (auto& index : indexes) {
        if (index.sorterIdent) {
            idents.push_back(*index.sorterIdent);
        }
        if (index.sideWritesIdent) {
            idents.push_back(*index.sideWritesIdent);
        }
        if (index.skippedRecordsIdent) {
            idents.push_back(*index.skippedRecordsIdent);
        }
        if (index.constraintViolationsIdent) {
            idents.push_back(*index.constraintViolationsIdent);
        }
    }
    if (indexBuildIdent) {
        idents.push_back(*indexBuildIdent);
    }
    return idents;
}

void dropIdentsAndDeregisterOnCommit(OperationContext* opCtx,
                                     const UUID& buildUUID,
                                     std::vector<std::string> idents) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [buildUUID, idents = std::move(idents)](OperationContext* opCtx,
                                                boost::optional<Timestamp> commitTs) {
            invariant(commitTs && !commitTs->isNull());
            auto dropTime = StorageEngine::DropTime{StorageEngine::StableTimestamp{*commitTs}};
            auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
            for (auto& ident : idents) {
                storageEngine->addDropPendingIdent(dropTime, std::make_shared<Ident>(ident));
            }
            _registry(opCtx->getServiceContext()).remove(buildUUID);
        });
}

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
             std::vector<IndexBuildInfo> indexes,
             boost::optional<std::string> indexBuildIdent) {
    // We create temporary interceptors to eagerly create all of the sidetables needed. These
    // interceptors need to outlive the WUOW so that WUOW rollback can safely access the lazy record
    // stores.
    std::deque<IndexBuildInterceptor> interceptors;

    auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, {dbName, collectionUUID}, AcquisitionPrerequisites::OperationType::kWrite),
        LockMode::MODE_X);
    CollectionWriter writer{opCtx, &coll};
    WriteUnitOfWork wuow{opCtx};
    auto writableColl = writer.getWritableCollection(opCtx);

    if (indexBuildIdent) {
        LazyRecordStore::createTable(opCtx, *indexBuildIdent);
    }

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

        interceptors.emplace_back(
            opCtx, index, LazyRecordStore::CreateMode::immediate, descriptor.unique());

        CollectionQueryInfo::get(writableColl).rebuildIndexData(opCtx, writableColl);

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
        [dbName = std::move(dbName),
         collectionUUID,
         buildUUID,
         indexes = std::move(indexes),
         indexBuildIdent = std::move(indexBuildIdent)](OperationContext* opCtx,
                                                       boost::optional<Timestamp>) mutable {
            _registry(opCtx->getServiceContext())
                .add(buildUUID,
                     std::move(dbName),
                     collectionUUID,
                     std::move(indexes),
                     std::move(indexBuildIdent));
        });

    wuow.commit();
    return Status::OK();
}

Status commit(OperationContext* opCtx,
              DatabaseName dbName,
              const UUID& collectionUUID,
              const UUID& buildUUID,
              const std::vector<IndexBuildInfo>& indexes,
              const std::vector<boost::optional<MultikeyPaths>>& multikey,
              boost::optional<std::string> indexBuildIdent) {
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

        writableColl->indexBuildSuccess(opCtx, entry);
        if (multikey[i]) {
            entry->setMultikey(opCtx, writer.get(), {}, *multikey[i]);
        }

        CollectionIndexUsageTrackerDecoration::write(writableColl)
            .unregisterIndex(index.getIndexName());
        CollectionIndexUsageTrackerDecoration::write(writableColl)
            .registerIndex(entry->descriptor()->indexName(),
                           entry->descriptor()->keyPattern(),
                           IndexFeatures::make(entry->descriptor(), coll.nss().isOnInternalDb()));

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

    dropIdentsAndDeregisterOnCommit(
        opCtx, buildUUID, getIndexBuildIdents(indexes, indexBuildIdent));
    opCtx->getServiceContext()->getOpObserver()->onCommitIndexBuild(
        opCtx,
        coll.nss(),
        collectionUUID,
        buildUUID,
        indexes,
        multikeyPathsToObjs(indexes, multikey),
        /*fromMigrate=*/false);
    wuow.commit();
    return Status::OK();
}

Status abort(OperationContext* opCtx,
             DatabaseName dbName,
             const UUID& collectionUUID,
             const UUID& buildUUID,
             const std::vector<IndexBuildInfo>& indexes,
             boost::optional<std::string> indexBuildIdent,
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

    dropIdentsAndDeregisterOnCommit(
        opCtx, buildUUID, getIndexBuildIdents(indexes, indexBuildIdent));
    opCtx->getServiceContext()->getOpObserver()->onAbortIndexBuild(opCtx,
                                                                   coll.nss(),
                                                                   collectionUUID,
                                                                   buildUUID,
                                                                   indexes,
                                                                   cause,
                                                                   /*fromMigrate=*/false);
    wuow.commit();
    return Status::OK();
}

ResumeIndexInfo resumeInfo(OperationContext* opCtx,
                           const UUID& collectionUUID,
                           const UUID& buildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           const std::string& ident) {
    uassert(ErrorCodes::InvalidOptions,
            "Invalid index build resume state ident",
            ident::isInternalIdent(ident, kIndexBuildIdentStem));

    auto resumeIndexInfoDoc = index_builds::readResumeIndexInfo(
        opCtx->getServiceContext()->getStorageEngine(), opCtx, ident);

    boost::optional<ResumeIndexInfo> resumeIndexInfo;
    if (resumeIndexInfoDoc) {
        resumeIndexInfo = index_builds::parseResumeIndexInfo(*resumeIndexInfoDoc);
    } else {
        // If we don't have a persisted resume state but we did have the index build in the
        // registry, synthesize an initial phase resume state so that index builds that are
        // interrupted prior to persisting their resume state can be handled properly.
        resumeIndexInfo = index_builds::synthesizeResumeIndexInfo(
            buildUUID, IndexBuildPhaseEnum::kInitialized, collectionUUID, indexes);
    }

    uassert(ErrorCodes::FailedToParse,
            "Failed to read/parse the index build resume state",
            resumeIndexInfo);

    return *resumeIndexInfo;
}

void deleteSorterEntriesOutsideRanges(OperationContext* opCtx,
                                      const std::vector<IndexStateInfo>& resumeInfoIndexes) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    for (const auto& indexStateInfo : resumeInfoIndexes) {
        auto storageId = indexStateInfo.getStorageIdentifier();
        if (!storageId) {
            continue;
        }

        LazyRecordStore sorterTable(opCtx, *storageId, LazyRecordStore::CreateMode::openExisting);
        auto& container = std::get<std::reference_wrapper<IntegerKeyedContainer>>(
                              sorterTable.getTableOrThrow().getContainer())
                              .get();

        const auto batchMaxSize =
            static_cast<size_t>(primaryDrivenIndexBuildSorterInsertionBatchSize.load());
        std::vector<int64_t> keysToDelete;
        int64_t numDeleted = 0;

        auto flushDeletes = [&] {
            writeConflictRetry(
                opCtx, ru, "deleteSorterEntriesOutsideRanges", NamespaceString::kEmpty, [&] {
                    Lock::GlobalLock lk(opCtx, MODE_IX);
                    WriteUnitOfWork wuow{opCtx};
                    for (auto key : keysToDelete) {
                        uassertStatusOK(container_write::remove(opCtx, ru, container, key));
                    }
                    wuow.commit();
                });
            numDeleted += keysToDelete.size();
            keysToDelete.clear();
        };

        auto collectKey = [&](int64_t key) {
            keysToDelete.push_back(key);
            if (keysToDelete.size() >= batchMaxSize) {
                flushDeletes();
            }
        };

        auto cursor = container.getCursor(ru);

        auto& ranges = indexStateInfo.getRanges();
        boost::optional<int64_t> firstStart;
        boost::optional<int64_t> lastEnd;

        if (!ranges || ranges->empty()) {
            // Delete every entry. Write conflicts may reset the cursor to the beginning which is
            // safe since we are deleting all keys and committed deletes never reappear.
            for (auto entry = cursor->next(); entry; entry = cursor->next()) {
                collectKey(entry->first);
            }
        } else {
            firstStart = ranges->front().getStart();
            lastEnd = ranges->back().getEnd();

            // Delete keys before firstStart. Write conflicts may reset the cursor to the beginning
            // which is safe since we only delete keys < firstStart.
            for (auto entry = cursor->next(); entry && entry->first < *firstStart;
                 entry = cursor->next()) {
                collectKey(entry->first);
            }

            // Delete keys at and after lastEnd. lastEnd is kept as a seek anchor and deleted last.
            // A key < lastEnd after flush indicates a write conflict reset the cursor so we re-seek
            // lastEnd.
            if (cursor->find(*lastEnd)) {
                while (auto entry = cursor->next()) {
                    if (entry->first < *lastEnd) {
                        cursor->find(*lastEnd);
                        continue;
                    }
                    collectKey(entry->first);
                }
                collectKey(*lastEnd);
            }
        }

        if (!keysToDelete.empty()) {
            flushDeletes();
        }

        LOGV2(12784900,
              "Index build: cleaned sorter entries outside persisted ranges",
              "sorterIdent"_attr = *storageId,
              "numDeleted"_attr = numDeleted,
              "firstRangeStart"_attr = firstStart,
              "lastRangeEnd"_attr = lastEnd);
    }
}

}  // namespace mongo::index_builds::primary_driven
