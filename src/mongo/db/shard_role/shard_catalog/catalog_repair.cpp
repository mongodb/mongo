// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/catalog_repair.h"

#include "mongo/db/index_builds/primary_driven/enabled.h"
#include "mongo/db/index_builds/resumable_index_builds_common.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/stdx/unordered_set.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

namespace mongo {
namespace {
/**
 * Returns whether the given ident is an internal ident and if it should be dropped or used to
 * resume an index build. We always return false for idents used by replicated fastcount, regardless
 * of shutdown state.
 */
bool identHandler(StorageEngine* engine,
                  OperationContext* opCtx,
                  const std::string& ident,
                  StorageEngine::LastShutdownState lastShutdownState,
                  StorageEngine::ReconcileResult* reconcileResult,
                  stdx::unordered_set<std::string>& internalIdentsToKeep,
                  stdx::unordered_set<std::string>& allInternalIdents) {
    if (!ident::isInternalIdent(ident) || ident::isReplicatedFastCountIdent(ident)) {
        // We don't need to insert replicated fastcount idents to allInternalIdents since that is
        // just used to drop idents that aren't also in internalIdentsToKeep.
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
    auto resumeInfo = index_builds::readAndParseResumeIndexInfo(engine, opCtx, ident);
    if (resumeInfo) {
        // Keep the tables that are needed to rebuild this index.
        // Note: the table that stores the rebuild metadata itself (i.e. |ident|) isn't kept.
        for (const mongo::IndexStateInfo& idx : resumeInfo->getIndexes()) {
            internalIdentsToKeep.insert(std::string{idx.getSideWritesTable()});
            if (idx.getDuplicateKeyTrackerTable()) {
                internalIdentsToKeep.insert(std::string{*idx.getDuplicateKeyTrackerTable()});
            }
            if (idx.getSkippedRecordTrackerTable()) {
                internalIdentsToKeep.insert(std::string{*idx.getSkippedRecordTrackerTable()});
            }
        }

        reconcileResult->indexBuildsToResume.push_back(std::move(*resumeInfo));

        return true;
    }
    return false;
}
}  // namespace

namespace catalog_repair {
using namespace std::literals::string_view_literals;

/**
 * This method reconciles differences between idents the KVEngine is aware of and the
 * MDBCatalog. There are four differences to consider:
 *
 * First, a KVEngine may know of an ident that the MDBCatalog does not. This method will drop
 * the ident from the KVEngine.
 *
 * Second, a MDBCatalog may have a collection ident that the KVEngine does not. This is an
 * illegal state and this method returns an error.
 *
 * Third, a MDBCatalog may have an index ident that the KVEngine does not. For ready indexes
 * this method logs the inconsistency. Unfinished two-phase builds are returned to the caller
 * for restart and other unfinished indexes are dropped, whether or not their idents exist.
 *
 * Fourth, a catalog entry may record no usable ident for an index it lists. This is an
 * illegal state and this method returns an error.
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
        if (auto it = engineIdents.find(ident::kMdbCatalog); it != engineIdents.end())
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

        const auto& toRemove = it;
        const Timestamp identDropTs = stableTs;
        LOGV2_PROD_ONLY(
            22251, "Dropping unknown ident", "ident"_attr = toRemove, "ts"_attr = identDropTs);
        engine->dropUnknownIdent(
            *shard_role_details::getRecoveryUnit(opCtx), identDropTs, toRemove);
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

    // Scan all indexes in the catalog. Every present index must record a usable ident.
    // Unfinished two-phase builds are returned to the caller for restart; other unfinished
    // indexes are dropped. A ready index whose ident the engine no longer has is logged.
    for (const MDBCatalog::EntryIdentifier& entry : catalogEntries) {
        const auto catalogEntry =
            durable_catalog::getParsedCatalogEntry(opCtx, entry.catalogId, mdbCatalog);
        auto md = catalogEntry->metadata;

        // Batch up the indexes to remove them from `metaData` outside of the iterator.
        std::vector<std::string> indexesToDrop;
        for (const auto& indexMetaData : md->indexes) {
            auto indexName = indexMetaData.nameStringData();
            auto indexIdentElem = catalogEntry->indexIdents.getField(indexName);

            // Every present index must have a non-empty ident. An empty string cannot be
            // acted on by dropIdent or index build restart, so such an entry is corrupt
            // and unsafe to start from (SERVER-128615).
            if (!forRepair && indexMetaData.isPresent() &&
                (indexIdentElem.type() != BSONType::string ||
                 indexIdentElem.valueStringData().empty())) {
                LOGV2_ERROR(12861500,
                            "Index in catalog entry has no usable ident",
                            logAttrs(md->nss),
                            "index"_attr = indexName,
                            "catalogId"_attr = entry.catalogId,
                            "collectionIdent"_attr = entry.ident,
                            "indexIdents"_attr = catalogEntry->indexIdents,
                            "metadata"_attr = md->toBSON(),
                            "lastShutdownState"_attr =
                                (lastShutdownState == StorageEngine::LastShutdownState::kClean
                                     ? "clean"sv
                                     : "unclean"sv));
                return {ErrorCodes::DataCorruptionDetected,
                        str::stream()
                            << "Expected index ident is missing from the catalog entry, "
                               "indicating catalog corruption (SERVER-128725). Remediate by "
                               "resyncing this node from a healthy replica set member or by "
                               "restoring from a backup. Collection: "
                            << md->nss.toStringForErrorMsg() << " Index: " << indexName};
            }

            const std::string indexIdent = indexIdentElem.str();

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
                        {buildUUID, {.dbName = entry.nss.dbName(), .collUUID = *collUUID}});
                    existingIt = reconcileResult.indexBuildsToRestart.find(buildUUID);
                }

                existingIt->second.indexSpecsAndIdents.emplace_back(indexMetaData.spec, indexIdent);
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
                engine->dropIdent(*shard_role_details::getRecoveryUnit(opCtx), indexIdent);
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
            writeConflictRetry(opCtx, "dropUnfinishedIndexes", entry.nss, [&] {
                WriteUnitOfWork wuow(opCtx);
                CollectionWriter writer{opCtx, entry.nss};
                auto collection = writer.getWritableCollection(opCtx);
                invariant(collection->getCatalogId() == entry.catalogId);
                collection->replaceMetadata(opCtx, md);
                wuow.commit();
            });
        }
    }

    if (index_builds::primary_driven::enabled(opCtx)) {
        return reconcileResult;
    }

    // Drop any internal ident that we won't need.
    for (auto&& temp : allInternalIdents) {
        if (internalIdentsToKeep.contains(temp)) {
            continue;
        }
        LOGV2(22257, "Dropping internal ident", "ident"_attr = temp);
        engine->dropIdent(*shard_role_details::getRecoveryUnit(opCtx), temp);
    }

    return reconcileResult;
}

}  // namespace catalog_repair
}  // namespace mongo
