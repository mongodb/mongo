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

#include "mongo/db/local_catalog/catalog_repair.h"

#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

namespace mongo {

MONGO_FAIL_POINT_DEFINE(failToParseResumeIndexInfo);

namespace {
/**
 * Returns whether the given ident is an internal ident and if it should be dropped or used to
 * resume an index build.
 */
bool identHandler(StorageEngine* engine,
                  OperationContext* opCtx,
                  const std::string& ident,
                  StorageEngine::LastShutdownState lastShutdownState,
                  StorageEngine::ReconcileResult* reconcileResult,
                  stdx::unordered_set<std::string>& internalIdentsToKeep,
                  stdx::unordered_set<std::string>& allInternalIdents) {
    if (!ident::isInternalIdent(ident)) {
        return false;
    }

    allInternalIdents.insert(ident);

    // When starting up after an unclean shutdown, we do not attempt to recover any state from the
    // internal idents. Thus, we drop them in this case.
    if (lastShutdownState == StorageEngine::LastShutdownState::kUnclean) {
        return true;
    }

    if (!ident::isInternalIdent(ident, kResumableIndexIdentStem)) {
        return false;
    }

    // When starting up after a clean shutdown and resumable index builds are supported, find the
    // internal idents that contain the relevant information to resume each index build and recover
    // the state.
    auto rs = engine->getEngine()->getRecordStore(
        opCtx, NamespaceString::kEmpty, ident, RecordStore::Options{}, boost::none /* uuid */);

    auto cursor = rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    auto record = cursor->next();
    if (record) {
        auto doc = record.value().data.toBson();

        // Parse the documents here so that we can restart the build if the document doesn't
        // contain all the necessary information to be able to resume building the index.
        ResumeIndexInfo resumeInfo;
        try {
            if (MONGO_unlikely(failToParseResumeIndexInfo.shouldFail())) {
                uasserted(ErrorCodes::FailPointEnabled,
                          "failToParseResumeIndexInfo fail point is enabled");
            }

            resumeInfo = ResumeIndexInfo::parse(doc, IDLParserContext("ResumeIndexInfo"));
        } catch (const DBException& e) {
            LOGV2(4916300, "Failed to parse resumable index info", "error"_attr = e.toStatus());

            // Ignore the error so that we can restart the index build instead of resume it. We
            // should drop the internal ident if we failed to parse.
            return true;
        }

        LOGV2(4916301,
              "Found unfinished index build to resume",
              "buildUUID"_attr = resumeInfo.getBuildUUID(),
              "collectionUUID"_attr = resumeInfo.getCollectionUUID(),
              "phase"_attr = IndexBuildPhase_serializer(resumeInfo.getPhase()));

        // Keep the tables that are needed to rebuild this index.
        // Note: the table that stores the rebuild metadata itself (i.e. |ident|) isn't kept.
        for (const mongo::IndexStateInfo& idx : resumeInfo.getIndexes()) {
            internalIdentsToKeep.insert(std::string{idx.getSideWritesTable()});
            if (idx.getDuplicateKeyTrackerTable()) {
                internalIdentsToKeep.insert(std::string{*idx.getDuplicateKeyTrackerTable()});
            }
            if (idx.getSkippedRecordTrackerTable()) {
                internalIdentsToKeep.insert(std::string{*idx.getSkippedRecordTrackerTable()});
            }
        }

        reconcileResult->indexBuildsToResume.push_back(std::move(resumeInfo));

        return true;
    }
    return false;
}
}  // namespace

namespace catalog_repair {

/**
 * This method reconciles differences between idents the KVEngine is aware of and the
 * MDBCatalog. There are three differences to consider:
 *
 * First, a KVEngine may know of an ident that the MDBCatalog does not. This method will drop
 * the ident from the KVEngine.
 *
 * Second, a MDBCatalog may have a collection ident that the KVEngine does not. This is an
 * illegal state and this method fasserts.
 *
 * Third, a MDBCatalog may have an index ident that the KVEngine does not. This method will
 * rebuild the index.
 */
StatusWith<StorageEngine::ReconcileResult> reconcileCatalogAndIdents(
    OperationContext* opCtx,
    StorageEngine* engine,
    Timestamp stableTs,
    StorageEngine::LastShutdownState lastShutdownState,
    bool forRepair) {
    auto mdbCatalog = engine->getMDBCatalog();

    // Gather all tables known to the storage engine and drop those that aren't cross-referenced
    // in the _mdb_catalog. This can happen for two reasons.
    //
    // First, collection creation and deletion happen in two steps. First the storage engine
    // creates/deletes the table, followed by the change to the _mdb_catalog. It's not assumed a
    // storage engine can make these steps atomic.
    //
    // Second, a replica set node in 3.6+ on supported storage engines will only persist "stable"
    // data to disk. That is data which replication guarantees won't be rolled back. The
    // _mdb_catalog will reflect the "stable" set of collections/indexes. However, it's not
    // expected for a storage engine's ability to persist stable data to extend to "stable
    // tables".
    std::set<std::string, std::less<>> engineIdents;
    {
        std::vector<std::string> vec =
            engine->getEngine()->getAllIdents(*shard_role_details::getRecoveryUnit(opCtx));
        for (auto& elem : vec)
            engineIdents.insert(std::move(elem));
        if (auto it = engineIdents.find(ident::kMbdCatalog); it != engineIdents.end())
            engineIdents.erase(it);
    }

    LOGV2_FOR_RECOVERY(4615633, 2, "Reconciling collection and index idents.");
    stdx::unordered_set<std::string> catalogIdents;
    {
        std::vector<std::string> vec = mdbCatalog->getAllIdents(opCtx);
        for (auto& elem : vec) {
            catalogIdents.insert(std::move(elem));
        }
    }
    stdx::unordered_set<std::string> internalIdentsToKeep;
    stdx::unordered_set<std::string> allInternalIdents;

    auto dropPendingIdents = engine->getDropPendingIdents();

    // Drop all idents in the storage engine that are not known to the catalog. This can happen in
    // the case of a collection or index creation being rolled back.
    StorageEngine::ReconcileResult reconcileResult;
    for (const auto& it : engineIdents) {
        if (catalogIdents.find(it) != catalogIdents.end()) {
            continue;
        }

        if (identHandler(engine,
                         opCtx,
                         it,
                         lastShutdownState,
                         &reconcileResult,
                         internalIdentsToKeep,
                         allInternalIdents)) {
            continue;
        }

        if (!ident::isCollectionOrIndexIdent(it)) {
            // Only indexes and collections are candidates for dropping when the storage engine's
            // metadata does not align with the catalog metadata.
            continue;
        }

        // In repair context, any orphaned collection idents from the engine should already be
        // recovered in the catalog in loadMDBCatalog().
        invariant(!(ident::isCollectionIdent(it) && forRepair));

        // Leave drop-pending idents alone.
        // These idents have to be retained as long as the corresponding drops are not part of a
        // checkpoint.
        if (dropPendingIdents.find(it) != dropPendingIdents.cend()) {
            LOGV2(22250,
                  "Not removing ident for uncheckpointed collection or index drop",
                  "ident"_attr = it);
            continue;
        }

        const auto& toRemove = it;
        const Timestamp identDropTs = stableTs;
        LOGV2_PROD_ONLY(
            22251, "Dropping unknown ident", "ident"_attr = toRemove, "ts"_attr = identDropTs);
        if (!identDropTs.isNull()) {
            engine->addDropPendingIdent(
                identDropTs, std::make_shared<Ident>(toRemove), /*onDrop=*/nullptr);
        } else {
            WriteUnitOfWork wuow(opCtx);
            Status status =
                engine->getEngine()->dropIdent(*shard_role_details::getRecoveryUnit(opCtx),
                                               toRemove,
                                               ident::isCollectionIdent(toRemove));
            if (!status.isOK()) {
                // A concurrent operation, such as a checkpoint could be holding an open data handle
                // on the ident. Handoff the ident drop to the ident reaper to retry later.
                engine->addDropPendingIdent(
                    identDropTs, std::make_shared<Ident>(toRemove), /*onDrop=*/nullptr);
            }
            wuow.commit();
        }
    }

    // Scan all collections in the catalog and make sure their ident is known to the storage
    // engine. An omission here is fatal. A missing ident could mean a collection drop was rolled
    // back. Note that startup already attempts to open tables; this should only catch errors in
    // other contexts such as `recoverToStableTimestamp`.
    std::vector<MDBCatalog::EntryIdentifier> catalogEntries =
        mdbCatalog->getAllCatalogEntries(opCtx);
    if (!forRepair) {
        for (const MDBCatalog::EntryIdentifier& entry : catalogEntries) {
            if (engineIdents.find(entry.ident) == engineIdents.end()) {
                return {ErrorCodes::UnrecoverableRollbackError,
                        str::stream()
                            << "Expected collection does not exist. Collection: "
                            << entry.nss.toStringForErrorMsg() << " Ident: " << entry.ident};
            }
        }
    }

    // Scan all indexes and return those in the catalog where the storage engine does not have the
    // corresponding ident. The caller is expected to rebuild these indexes.
    //
    // Also, remove unfinished builds except those that were background index builds started on a
    // secondary.
    for (const MDBCatalog::EntryIdentifier& entry : catalogEntries) {
        const auto catalogEntry =
            durable_catalog::getParsedCatalogEntry(opCtx, entry.catalogId, mdbCatalog);
        auto md = catalogEntry->metadata;

        // Batch up the indexes to remove them from `metaData` outside of the iterator.
        std::vector<std::string> indexesToDrop;
        for (const auto& indexMetaData : md->indexes) {
            auto indexName = indexMetaData.nameStringData();
            auto indexIdent = mdbCatalog->getIndexIdent(opCtx, entry.catalogId, indexName);

            // Warn in case of incorrect "multikeyPath" information in catalog documents. This is
            // the result of a concurrency bug which has since been fixed, but may persist in
            // certain catalog documents. See https://jira.mongodb.org/browse/SERVER-43074
            const bool hasMultiKeyPaths =
                std::any_of(indexMetaData.multikeyPaths.begin(),
                            indexMetaData.multikeyPaths.end(),
                            [](auto& pathSet) { return pathSet.size() > 0; });
            if (!indexMetaData.multikey && hasMultiKeyPaths) {
                LOGV2_WARNING(
                    22267,
                    "The 'multikey' field for index was false with non-empty 'multikeyPaths'. This "
                    "indicates corruption of the catalog. Consider either dropping and recreating "
                    "the index, or rerunning with the --repair option. See "
                    "http://dochub.mongodb.org/core/repair for more information",
                    "index"_attr = indexName,
                    logAttrs(md->nss));
            }

            if (!engineIdents.count(indexIdent)) {
                // There are certain cases where the catalog entry may reference an index ident
                // which is no longer present. One example of this is when an unclean shutdown
                // occurs before a checkpoint is taken during startup recovery. Since we drop the
                // index ident without a timestamp when restarting the index build for startup
                // recovery, the subsequent startup recovery can see the now-dropped ident
                // referenced by the old index catalog entry.
                LOGV2(6386500,
                      "Index catalog entry ident not found",
                      "ident"_attr = indexIdent,
                      "entry"_attr = indexMetaData.spec,
                      logAttrs(md->nss));
            }

            // Any index build with a UUID is an unfinished two-phase build and must be restarted.
            // There are no special cases to handle on primaries or secondaries. An index build may
            // be associated with multiple indexes. We should only restart an index build if we
            // aren't going to resume it.
            if (indexMetaData.buildUUID) {
                invariant(!indexMetaData.ready);

                auto collUUID = md->options.uuid;
                invariant(collUUID);
                auto buildUUID = *indexMetaData.buildUUID;

                LOGV2(22253,
                      "Found index from unfinished build",
                      logAttrs(md->nss),
                      "uuid"_attr = *collUUID,
                      "index"_attr = indexName,
                      "buildUUID"_attr = buildUUID);

                // Insert in the map if a build has not already been registered.
                auto existingIt = reconcileResult.indexBuildsToRestart.find(buildUUID);
                if (existingIt == reconcileResult.indexBuildsToRestart.end()) {
                    reconcileResult.indexBuildsToRestart.insert(
                        {buildUUID, IndexBuildsEntry(*collUUID)});
                    existingIt = reconcileResult.indexBuildsToRestart.find(buildUUID);
                }

                existingIt->second.indexSpecs.emplace_back(indexMetaData.spec);
                continue;
            }

            // The last anomaly is when the index build did not complete. This implies the index
            // build was on:
            // (1) a standalone and the `createIndexes` command never successfully returned, or
            // (2) an initial syncing node bulk building indexes during a collection clone.
            // In both cases the index entry in the catalog should be dropped.
            if (!indexMetaData.ready) {
                LOGV2(22256,
                      "Dropping unfinished index",
                      logAttrs(md->nss),
                      "index"_attr = indexName);
                // Ensure the `ident` is dropped while we have the `indexIdent` value.
                Status status =
                    engine->getEngine()->dropIdent(*shard_role_details::getRecoveryUnit(opCtx),
                                                   indexIdent,
                                                   /*identHasSizeInfo=*/false);
                if (!status.isOK()) {
                    // A concurrent operation, such as a checkpoint could be holding an open data
                    // handle on the ident. Handoff the ident drop to the ident reaper to retry
                    // later.
                    engine->addDropPendingIdent(
                        Timestamp::min(), std::make_shared<Ident>(indexIdent), /*onDrop=*/nullptr);
                }
                indexesToDrop.push_back(std::string{indexName});
                continue;
            }
        }

        for (auto&& indexName : indexesToDrop) {
            invariant(md->eraseIndex(indexName),
                      str::stream() << "Index is missing. Collection: "
                                    << md->nss.toStringForErrorMsg() << " Index: " << indexName);
        }
        if (indexesToDrop.size() > 0) {
            WriteUnitOfWork wuow(opCtx);
            CollectionWriter writer{opCtx, entry.nss};
            auto collection = writer.getWritableCollection(opCtx);
            invariant(collection->getCatalogId() == entry.catalogId);
            collection->replaceMetadata(opCtx, std::move(md));
            wuow.commit();
        }
    }

    // Drop any internal ident that we won't need.
    for (auto&& temp : allInternalIdents) {
        if (internalIdentsToKeep.contains(temp)) {
            continue;
        }
        LOGV2(22257, "Dropping internal ident", "ident"_attr = temp);
        WriteUnitOfWork wuow(opCtx);
        Status status = engine->getEngine()->dropIdent(
            *shard_role_details::getRecoveryUnit(opCtx), temp, ident::isCollectionIdent(temp));
        if (!status.isOK()) {
            // A concurrent operation, such as a checkpoint could be holding an open data handle on
            // the ident. Handoff the ident drop to the ident reaper to retry later.
            engine->addDropPendingIdent(
                Timestamp::min(), std::make_shared<Ident>(temp), /*onDrop=*/nullptr);
        }
        wuow.commit();
    }

    return reconcileResult;
}

}  // namespace catalog_repair
}  // namespace mongo
