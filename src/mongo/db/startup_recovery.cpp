/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/startup_recovery.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/index_builds/rebuild_indexes.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_repair.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repair.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_set_member_in_standalone_mode.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

// Exit after repair has started, but before data is repaired.
MONGO_FAIL_POINT_DEFINE(exitBeforeDataRepair);
// Exit after repairing data, but before the replica set configuration is invalidated.
MONGO_FAIL_POINT_DEFINE(exitBeforeRepairInvalidatesConfig);

// Returns true if storage engine is writable.
bool isWriteableStorageEngine() {
    return storageGlobalParams.engine != "devnull";
}

// Attempt to restore the featureCompatibilityVersion document if it is missing.
// The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
// this function into one single builder that records the time elapsed during startup. Its default
// value is nullptr because we only want to time this function when it is called during startup.
Status restoreMissingFeatureCompatibilityVersionDocument(
    OperationContext* opCtx, BSONObjBuilder* startupTimeElapsedBuilder = nullptr) {
    const NamespaceString fcvNss(NamespaceString::kServerConfigurationNamespace);

    // If the admin database, which contains the server configuration collection with the
    // featureCompatibilityVersion document, does not exist, create it.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, fcvNss.dbName());
    if (!db) {
        LOGV2(20998, "Re-creating admin database that was dropped.");
    }
    db = databaseHolder->openDb(opCtx, fcvNss.dbName());
    invariant(db);

    // If the server configuration collection, which contains the FCV document, does not exist, then
    // create it.
    auto catalog = CollectionCatalog::get(opCtx);
    if (!catalog->lookupCollectionByNamespace(opCtx, fcvNss)) {
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        LOGV2(4926905,
              "Re-creating featureCompatibilityVersion document that was deleted. Creating new "
              "document with last LTS version.",
              "version"_attr = multiversion::toString(multiversion::GenericFCV::kLastLTS));
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::createFCVDocument,
                                       startupTimeElapsedBuilder);
        uassertStatusOK(createCollection(opCtx, fcvNss.dbName(), BSON("create" << fcvNss.coll())));
    }

    const auto fcvColl = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(fcvNss,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);
    invariant(fcvColl.exists());

    // Restore the featureCompatibilityVersion document if it is missing.
    BSONObj featureCompatibilityVersion;
    if (!Helpers::findOne(opCtx,
                          fcvColl,
                          BSON("_id" << multiversion::kParameterName),
                          featureCompatibilityVersion)) {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::restoreFCVDocument,
                                       startupTimeElapsedBuilder);
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        LOGV2(21000,
              "Re-creating featureCompatibilityVersion document that was deleted. Creating new "
              "document with version ",
              "version"_attr = multiversion::toString(multiversion::GenericFCV::kLastLTS));

        FeatureCompatibilityVersionDocument fcvDoc;
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        fcvDoc.setVersion(multiversion::GenericFCV::kLastLTS);

        writeConflictRetry(opCtx, "insertFCVDocument", fcvNss, [&] {
            WriteUnitOfWork wunit(opCtx);
            uassertStatusOK(Helpers::insert(opCtx, fcvColl, fcvDoc.toBSON()));
            wunit.commit();
        });
    }

    invariant(Helpers::findOne(
        opCtx, fcvColl, BSON("_id" << multiversion::kParameterName), featureCompatibilityVersion));

    return Status::OK();
}

/**
 * Returns true if the collection associated with the given CollectionCatalogEntry has an index on
 * the _id field
 */
bool checkIdIndexExists(OperationContext* opCtx, const Collection* coll) {
    auto indexNames = std::vector<std::string>();
    coll->getAllIndexes(&indexNames);

    for (const auto& name : indexNames) {
        if (name == IndexConstants::kIdIndexName) {
            return true;
        }
    }
    return false;
}

Status buildMissingIdIndex(OperationContext* opCtx, const NamespaceString nss) {
    MultiIndexBlock indexer;
    // This method is called in startup recovery so we can safely build the id index in foreground
    // mode. This prevents us from yielding a MODE_X lock (which is disallowed).
    indexer.setIndexBuildMethod(IndexBuildMethodEnum::kForeground);
    CollectionWriter collWriter(opCtx, nss);
    LOGV2(4805002, "Building missing _id index", logAttrs(*collWriter));

    ScopeGuard abortOnExit(
        [&] { indexer.abortIndexBuild(opCtx, collWriter, MultiIndexBlock::kNoopOnCleanUpFn); });

    const auto indexCatalog = collWriter->getIndexCatalog();
    IndexBuildInfo idIndexBuildInfo(indexCatalog->getDefaultIdIndexSpec(collWriter.get()),
                                    *opCtx->getServiceContext()->getStorageEngine(),
                                    nss.dbName(),
                                    VersionContext::getDecoration(opCtx));
    auto swSpecs = indexer.init(opCtx,
                                collWriter,
                                {std::move(idIndexBuildInfo)},
                                MultiIndexBlock::kNoopOnInitFn,
                                MultiIndexBlock::InitMode::SteadyState);
    if (!swSpecs.isOK()) {
        return swSpecs.getStatus();
    }

    auto status = indexer.insertAllDocumentsInCollection(opCtx, nss);
    if (!status.isOK()) {
        return status;
    }

    status = indexer.checkConstraints(opCtx, collWriter.get());
    if (!status.isOK()) {
        return status;
    }

    WriteUnitOfWork wuow(opCtx);
    status = indexer.commit(opCtx,
                            collWriter.getWritableCollection(opCtx),
                            MultiIndexBlock::kNoopOnCreateEachFn,
                            MultiIndexBlock::kNoopOnCommitFn);
    wuow.commit();
    abortOnExit.dismiss();
    return status;
}

auto downgradeError =
    Status{ErrorCodes::MustDowngrade,
           str::stream() << "UPGRADE PROBLEM: The data files need to be fully upgraded to version "
                            "4.4 before attempting a binary upgrade; see "
                         << feature_compatibility_version_documentation::upgradeLink()
                         << " for more details."};

/**
 * Checks that all collections on a database have valid properties for this version of MongoDB.
 *
 * This validates that required collections have an _id index. If a collection is missing an _id
 * index, this function will build it if EnsureIndexPolicy is kBuildMissing.
 *
 * Returns a MustDowngrade error if any index builds on the required _id field fail.
 */
enum class EnsureIndexPolicy { kBuildMissing, kError };
Status ensureCollectionProperties(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  EnsureIndexPolicy ensureIndexPolicy) {
    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& coll : catalog->range(dbName)) {
        if (!coll) {
            break;
        }

        // All user-created replicated collections created since MongoDB 4.0 have _id indexes.
        auto requiresIndex = coll->requiresIdIndex() && coll->ns().isReplicated();
        const auto& collOptions = coll->getCollectionOptions();
        auto hasAutoIndexIdField = collOptions.autoIndexId == CollectionOptions::YES;

        // Even if the autoIndexId field is not YES, the collection may still have an _id index
        // that was created manually by the user. Check the list of indexes to confirm index
        // does not exist before attempting to build it or returning an error.
        if (requiresIndex && !hasAutoIndexIdField && !checkIdIndexExists(opCtx, coll)) {
            LOGV2(21001, "Collection is missing an _id index", logAttrs(*coll));
            if (EnsureIndexPolicy::kBuildMissing == ensureIndexPolicy) {
                auto status = buildMissingIdIndex(opCtx, coll->ns());
                if (!status.isOK()) {
                    LOGV2_ERROR(21021,
                                "Could not build an _id index on collection",
                                logAttrs(*coll),
                                "error"_attr = status);
                    return downgradeError;
                }
            } else {
                return downgradeError;
            }
        }

        if (coll->getTimeseriesOptions() &&
            timeseries::collectionMayRequireExtendedRangeSupport(opCtx, *coll)) {
            coll->setRequiresTimeseriesExtendedRangeSupport(opCtx);
        }
    }
    return Status::OK();
}

/**
 * Opens each database and provides a callback on each one.
 */
template <typename Func>
void openDatabases(OperationContext* opCtx, Func&& onDatabase) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto dbNames = catalog::listDatabases();
    for (const auto& dbName : dbNames) {
        LOGV2_DEBUG(21010, 1, "    Opening database: {dbName}", "dbName"_attr = dbName);
        auto db = databaseHolder->openDb(opCtx, dbName);
        invariant(db);
        onDatabase(db->name());
    }
}

/**
 * Returns 'true' if this server has a configuration document in local.system.replset.
 */
bool hasReplSetConfigDoc(OperationContext* opCtx) {
    auto databaseHolder = DatabaseHolder::get(opCtx);

    // We open the "local" database before reading to ensure the in-memory catalog entries for the
    // 'kSystemReplSetNamespace' collection have been populated if the collection exists. If the
    // "local" database doesn't exist at this point yet, then it will be created.
    const auto nss = NamespaceString::kSystemReplSetNamespace;

    databaseHolder->openDb(opCtx, nss.dbName());
    BSONObj config;
    return Helpers::getSingleton(opCtx, nss, config);
}

/**
 * Check that the oplog is capped, and abort the process if it is not.
 * Caller must lock DB before calling this function.
 */
void assertCappedOplog(OperationContext* opCtx) {
    const NamespaceString oplogNss(NamespaceString::kRsOplogNamespace);
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(oplogNss.dbName(), MODE_IS));
    const Collection* oplogCollection =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, oplogNss);
    if (oplogCollection && !oplogCollection->isCapped()) {
        LOGV2_FATAL_NOTRACE(40115,
                            "The oplog collection is not capped; a capped oplog is a "
                            "requirement for replication to function.",
                            "oplogNamespace"_attr = oplogNss);
    }
}

void clearTempFilesExceptForResumableBuilds(const std::vector<ResumeIndexInfo>& indexBuildsToResume,
                                            const boost::filesystem::path& tempDir) {
    StringSet resumableIndexFiles;
    for (const auto& resumeInfo : indexBuildsToResume) {
        const auto& indexes = resumeInfo.getIndexes();
        for (const auto& index : indexes) {
            boost::optional<StringData> indexFilename = index.getFileName();
            if (indexFilename) {
                resumableIndexFiles.insert(std::string{*indexFilename});
            }
        }
    }

    size_t numFilesRemaining{0};
    auto dirItr = boost::filesystem::directory_iterator(tempDir);
    auto dirEnd = boost::filesystem::directory_iterator();
    for (; dirItr != dirEnd; ++dirItr) {
        auto curFilename = dirItr->path().filename().string();
        // Skip deleting files specified by 'resumableIndexFiles' and any directories. Specifically,
        // ensure that the StorageGlobalParams::getSpillDbPath() directory is not deleted.
        if (!resumableIndexFiles.contains(curFilename) &&
            !boost::filesystem::is_directory(dirItr->path())) {
            boost::system::error_code ec;
            boost::filesystem::remove(dirItr->path(), ec);
            if (ec) {
                LOGV2(5676601,
                      "Failed to clear temp directory file",
                      "filename"_attr = curFilename,
                      "error"_attr = ec.message());
            }
        } else {
            ++numFilesRemaining;
        }
    }

    if (numFilesRemaining == 0) {
        boost::system::error_code ec;
        boost::filesystem::remove_all(tempDir, ec);
        if (ec) {
            LOGV2(5071101, "Failed to clear temp directory", "error"_attr = ec.message());
        }
    }
}

void cleanupChangeCollectionAfterUncleanShutdown(OperationContext* opCtx,
                                                 const TenantId& tenantId) {
    auto currentTime =
        change_stream_serverless_helpers::getCurrentTimeForChangeCollectionRemoval(opCtx);
    auto expireAfterSeconds =
        Seconds{change_stream_serverless_helpers::getExpireAfterSeconds(tenantId)};

    // Change collection entries monotonically increase according to the 'ts' field of each entry's
    // corresponding oplog entry. However, the expiration is determined by the 'wall' time
    // reserved for each oplog entry. Since writes can commit out of order with respect to their
    // reserved 'wall' time (e.g. oplog holes), the change collection may not be strictly
    // monotonically increasing with respect to the 'wall' time.
    //
    // After unclean shutdown, initial truncate markers for the change collection don't exist yet.
    // Without truncate markers, truncation must be based on RecordId, which requires converting the
    // would-be 'wall' time expiry date into a Timestamp.
    //
    // To account for oplog holes, and a loss of precision converting Date_t to Timestamp,
    // truncate up to a few seconds past the expiration date to guarantee only consistent
    // data survives post crash.
    auto baseExpirationTime = currentTime - expireAfterSeconds;
    auto adjustedExpirationTime = baseExpirationTime +
        Seconds(startup_recovery::kChangeStreamPostUncleanShutdownExpiryExtensionSeconds);
    auto expirationTSEstimate = Timestamp(adjustedExpirationTime.toMillisSinceEpoch() / 1000.0,
                                          std::numeric_limits<unsigned>::max());
    RecordId maxExpiredRecordIdEstimate =
        record_id_helpers::keyForOptime(expirationTSEstimate, KeyFormat::String).getValue();

    writeConflictRetry(
        opCtx,
        "truncate change collection by approximate timestamp expiration",
        NamespaceString::makeChangeCollectionNSS(tenantId),
        [&] {
            AutoGetChangeCollection tenantChangeCollection{
                opCtx, AutoGetChangeCollection::AccessMode::kUnreplicatedWrite, tenantId};

            if (!tenantChangeCollection) {
                return;
            }

            LOGV2_DEBUG(
                8148800,
                0,
                "About to truncate change collection entries for tenant after unclean shutdown",
                "originalExpirationDate"_attr = baseExpirationTime,
                "newExpirationDate"_attr = adjustedExpirationTime,
                "expiryRangeExtensionSeconds"_attr =
                    startup_recovery::kChangeStreamPostUncleanShutdownExpiryExtensionSeconds,
                "truncateAtTimestamp"_attr = expirationTSEstimate,
                "tenantId"_attr = tenantId);

            // Exclusively truncate based on the most recent WT snapshot.
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            shard_role_details::getRecoveryUnit(opCtx)->allowOneUntimestampedWrite();

            WriteUnitOfWork wuow(opCtx);

            // Truncation is based on Timestamp expiration approximation -
            // meaning there isn't a good estimate of the number of bytes and
            // documents to be truncated, so default to 0.
            auto rs = tenantChangeCollection->getRecordStore();
            auto status = rs->rangeTruncate(opCtx,
                                            *shard_role_details::getRecoveryUnit(opCtx),
                                            RecordId(),
                                            maxExpiredRecordIdEstimate,
                                            0,
                                            0);
            invariantStatusOK(status);
            wuow.commit();
        });
}

// Returns the timestamp at which pre-images should be truncated for recovery.
Timestamp getPreImageTruncateTimestampForRecovery(OperationContext* opCtx) {
    const auto currentTime = change_stream_pre_image_util::getCurrentTimeForPreImageRemoval(opCtx);
    auto originalExpirationDate =
        change_stream_pre_image_util::getPreImageOpTimeExpirationDate(opCtx, currentTime);
    boost::optional<Date_t> operationTimeExpirationDate = boost::none;
    if (originalExpirationDate) {
        // Pre-image expiration is based on either 'operationTime', or '_id.ts'. However, after
        // unclean shutdown, initial truncation must be based on RecordId (which only encodes
        // '_id.ts') since truncate markers haven't been created yet. A pre-image's
        // 'operationTime' is based off it's corresponding oplog entry's reserved 'opTime'.
        // Because writes can commit out of order with respect to their reserved 'opTime' (e.g.
        // oplog holes), the pre-images collection may not be strictly monotonically increasing
        // with respect to 'operationTime'.
        //
        // To account for oplog holes, and a loss of precision converting Date_t to Timestamp,
        // truncate up to a few seconds past the expiration date to guarantee only consistent
        // data survives post crash.
        operationTimeExpirationDate = *originalExpirationDate +
            Seconds(startup_recovery::kChangeStreamPostUncleanShutdownExpiryExtensionSeconds);
    }

    auto operationTimeExpirationTSEstimate = operationTimeExpirationDate
        ? Timestamp{Timestamp(operationTimeExpirationDate->toMillisSinceEpoch() / 1000,
                              std::numeric_limits<unsigned>::max())}
        : Timestamp();

    // Pre-images expired when "_id.ts" < oldest oplog timestamp OR "_id.ts" <= the
    // estimated timestamp for the 'operationTime' expiration date.
    const auto oldestOplogTimestamp =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);
    const auto expirationTimestamp =
        std::max(oldestOplogTimestamp, operationTimeExpirationTSEstimate);
    LOGV2_DEBUG(7803703,
                0,
                "Computed timestamp to truncate pre-images at after unclean shutdown",
                "truncateAtTimestamp"_attr = expirationTimestamp,
                "oldestOplogEntryTimestamp"_attr = oldestOplogTimestamp);
    return expirationTimestamp;
}

void cleanupPreImagesCollectionAfterUncleanShutdown(OperationContext* opCtx) {
    writeConflictRetry(
        opCtx,
        "cleanupPreImagesCollectionAfterUncleanShutdown",
        NamespaceString::kChangeStreamPreImagesNamespace,
        [&] {
            shard_role_details::getRecoveryUnit(opCtx)->allowAllUntimestampedWrites();
            const auto preImagesColl =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      NamespaceString::kChangeStreamPreImagesNamespace,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kUnreplicatedWrite),
                                  MODE_IX);

            if (!preImagesColl.exists()) {
                LOGV2_DEBUG(7803702,
                            3,
                            "Bypassing truncation of pre-images collection on startup recovery "
                            "because it does not exist");
                return;
            }

            const auto approximateExpirationTimestamp =
                getPreImageTruncateTimestampForRecovery(opCtx);
            change_stream_pre_image_util::truncatePreImagesByTimestampExpirationApproximation(
                opCtx, preImagesColl, approximateExpirationTimestamp);
        });
}

// The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
// this function into one single builder that records the time elapsed during startup. Its default
// value is nullptr because we only want to time this function when it is called during startup.
void reconcileCatalogAndRestartUnfinishedIndexBuilds(
    OperationContext* opCtx,
    StorageEngine* storageEngine,
    StorageEngine::LastShutdownState lastShutdownState,
    BSONObjBuilder* startupTimeElapsedBuilder = nullptr) {

    StorageEngine::ReconcileResult reconcileResult;
    {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::dropAbandonedIdents,
                                       startupTimeElapsedBuilder);
        reconcileResult =
            fassert(40593,
                    catalog_repair::reconcileCatalogAndIdents(opCtx,
                                                              storageEngine,
                                                              storageEngine->getStableTimestamp(),
                                                              lastShutdownState,
                                                              storageGlobalParams.repair));
    }

    auto tempDir = boost::filesystem::path(storageGlobalParams.dbpath).append("_tmp");
    if (reconcileResult.indexBuildsToResume.empty() ||
        lastShutdownState == StorageEngine::LastShutdownState::kUnclean) {
        // If we did not find any index builds to resume or we are starting up after an unclean
        // shutdown, nothing in the temp directory will be used. Thus, we can clear it
        // completely.
        LOGV2(5071100, "Clearing temp directory");

        if (boost::filesystem::exists(tempDir)) {
            clearTempFilesExceptForResumableBuilds({}, tempDir);
        }
    } else if (boost::filesystem::exists(tempDir)) {
        // Clears the contents of the temp directory except for files for resumable builds.
        LOGV2(5676600, "Clearing temp directory except for files for resumable builds");

        clearTempFilesExceptForResumableBuilds(reconcileResult.indexBuildsToResume, tempDir);
    }

    if (auto* spillEngine = storageEngine->getSpillEngine()) {
        auto ru = spillEngine->newRecoveryUnit();
        auto idents = spillEngine->getAllIdents(*ru);
        LOGV2(10682200, "Dropping spill idents", "numIdents"_attr = idents.size());
        for (auto&& ident : idents) {
            auto result = spillEngine->dropIdent(*ru, ident, false);
            if (!result.isOK()) {
                LOGV2(10682201,
                      "Failed to drop spill table ident during startup recovery",
                      "ident"_attr = ident,
                      "error"_attr = result);
            }
        }
    }

    // Two-phase index builds depend on an eventually-replicated 'commitIndexBuild' oplog entry to
    // complete. Therefore, when a replica set member is started in standalone mode, we cannot
    // restart the index build because it will never complete.
    if (getReplSetMemberInStandaloneMode(opCtx->getServiceContext())) {
        LOGV2(
            9871800,
            "Any unfinished index builds will not be resumed nor restarted due to standalone mode",
            "numIndexBuildsToRestart"_attr = reconcileResult.indexBuildsToRestart.size(),
            "numIndexBuildsToResume"_attr = reconcileResult.indexBuildsToResume.size());
        return;
    }

    // Restart any unfinished index builds. This will not build any indexes to completion, but
    // rather start the background thread to build the index, and wait for a replicated commit or
    // abort oplog entry.
    IndexBuildsCoordinator::get(opCtx)->restartIndexBuildsForRecovery(
        opCtx, reconcileResult.indexBuildsToRestart, reconcileResult.indexBuildsToResume);
}

/**
 * Sets the appropriate flag on the service context decorable 'replSetMemberInStandaloneMode' to
 * 'true' if this is a replica set node running in standalone mode.
 */
void setReplSetMemberInStandaloneMode(OperationContext* opCtx, StorageEngine* engine) {
    if (repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        return;
    }

    invariant(shard_role_details::getLocker(opCtx)->isW());
    const Collection* collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
        opCtx, NamespaceString::kSystemReplSetNamespace);
    if (collection && !collection->isEmpty(opCtx)) {
        setReplSetMemberInStandaloneMode(opCtx->getServiceContext(), true);
        engine->setInStandaloneMode();
    }
}

// Perform startup procedures for --repair mode.
// The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
// this function into one single builder that records the time elapsed during startup. Its default
// value is nullptr because we only want to time this function when it is called during startup.
void startupRepair(OperationContext* opCtx,
                   StorageEngine* storageEngine,
                   BSONObjBuilder* startupTimeElapsedBuilder = nullptr) {
    invariant(!storageGlobalParams.queryableBackupMode);
    ServiceContext* svcCtx = opCtx->getServiceContext();

    if (MONGO_unlikely(exitBeforeDataRepair.shouldFail())) {
        LOGV2(21006, "Exiting because 'exitBeforeDataRepair' fail point was set.");
        quickExit(ExitCode::abrupt);
    }

    auto invalidateReplConfigIfNeeded = [opCtx]() {
        // If the config doesn't exist, don't invalidate anything. If this node were originally
        // part of a replica set but lost its config due to a repair, it would automatically
        // perform a resync. If this node is a standalone, this would lead to a confusing error
        // message if it were added to a replica set later on.
        auto storage = repl::StorageInterface::get(opCtx);
        auto swConfig = storage->findSingleton(opCtx, NamespaceString::kSystemReplSetNamespace);
        if (!swConfig.isOK()) {
            return;
        }
        auto config = swConfig.getValue();
        if (config.hasField(repl::ReplSetConfig::kRepairedFieldName)) {
            return;
        }
        BSONObjBuilder configBuilder(config);
        configBuilder.append(repl::ReplSetConfig::kRepairedFieldName, true);
        fassert(7101800,
                storage->putSingleton(opCtx,
                                      NamespaceString::kSystemReplSetNamespace,
                                      {configBuilder.obj(), Timestamp{}}));

        JournalFlusher::get(opCtx)->waitForJournalFlush();
    };

    // Repair, restore, and initialize the featureCompatibilityVersion document before allowing
    // repair to potentially rebuild indexes on the remaining collections. This ensures any
    // FCV-dependent features are rebuilt properly. Note that we don't try to prevent
    // repairDatabase from repairing this collection again, because it only consists of one
    // document.
    // If we fail to load the FCV document due to upgrade problems, we need to abort the repair in
    // order to allow downgrading to older binary versions.
    ScopeGuard abortRepairOnFCVErrors([&] {
        StorageRepairObserver::get(opCtx->getServiceContext())
            ->onRepairDone(opCtx, invalidateReplConfigIfNeeded);
    });

    auto catalog = CollectionCatalog::get(opCtx);
    if (auto fcvColl = catalog->lookupCollectionByNamespace(
            opCtx, NamespaceString::kServerConfigurationNamespace)) {
        auto databaseHolder = DatabaseHolder::get(opCtx);

        SectionScopedTimer scopedTimer(svcCtx->getFastClockSource(),
                                       TimedSectionId::repairServerConfigNamespace,
                                       startupTimeElapsedBuilder);
        databaseHolder->openDb(opCtx, fcvColl->ns().dbName());
        fassertNoTrace(4805000,
                       repair::repairCollection(
                           opCtx, storageEngine, NamespaceString::kServerConfigurationNamespace));
    }
    uassertStatusOK(
        restoreMissingFeatureCompatibilityVersionDocument(opCtx, startupTimeElapsedBuilder));

    {
        SectionScopedTimer scopedTimer(
            svcCtx->getFastClockSource(), TimedSectionId::initializeFCV, startupTimeElapsedBuilder);
        FeatureCompatibilityVersion::initializeForStartup(opCtx);
        abortRepairOnFCVErrors.dismiss();
    }

    // The local database should be repaired before any other replicated collections so we know
    // whether not to rebuild unfinished two-phase index builds if this is a replica set node
    // running in standalone mode.
    auto dbNames = catalog::listDatabases();
    if (auto it = std::find(dbNames.begin(), dbNames.end(), DatabaseName::kLocal);
        it != dbNames.end()) {
        SectionScopedTimer scopedTimer(
            svcCtx->getFastClockSource(), TimedSectionId::repairLocalDB, startupTimeElapsedBuilder);
        fassertNoTrace(4805001, repair::repairDatabase(opCtx, storageEngine, *it));

        // This must be set before rebuilding index builds on replicated collections.
        setReplSetMemberInStandaloneMode(opCtx, storageEngine);
        dbNames.erase(it);
    }

    {
        // Repair the remaining databases.
        SectionScopedTimer scopedTimer(svcCtx->getFastClockSource(),
                                       TimedSectionId::repairRemainingDB,
                                       startupTimeElapsedBuilder);
        for (const auto& dbName : dbNames) {
            fassertNoTrace(18506, repair::repairDatabase(opCtx, storageEngine, dbName));
        }
    }

    openDatabases(opCtx, [&](auto dbName) {
        // Ensures all collections meet requirements such as having _id indexes, and corrects them
        // if needed.
        uassertStatusOK(
            ensureCollectionProperties(opCtx, dbName, EnsureIndexPolicy::kBuildMissing));
    });

    if (MONGO_unlikely(exitBeforeRepairInvalidatesConfig.shouldFail())) {
        LOGV2(21008, "Exiting because 'exitBeforeRepairInvalidatesConfig' fail point was set.");
        quickExit(ExitCode::abrupt);
    }

    auto repairObserver = StorageRepairObserver::get(opCtx->getServiceContext());
    repairObserver->onRepairDone(opCtx, invalidateReplConfigIfNeeded);
    if (repairObserver->getModifications().size() > 0) {
        const auto& mods = repairObserver->getModifications();
        for (const auto& mod : mods) {
            LOGV2_WARNING(21019, "repairModification", "description"_attr = mod.getDescription());
        }
    }
    if (repairObserver->isDataInvalidated()) {
        if (hasReplSetConfigDoc(opCtx)) {
            LOGV2_WARNING(21020,
                          "WARNING: Repair may have modified replicated data. This node will no "
                          "longer be able to join a replica set without a full re-sync");
        }
    }

    // There were modifications, but only benign ones.
    if (repairObserver->getModifications().size() > 0 && !repairObserver->isDataInvalidated()) {
        LOGV2(21009,
              "Repair has made modifications to unreplicated data. The data is healthy and "
              "the node is eligible to be returned to the replica set.");
    }
}

// Perform collection validation for singular collection
bool offlineValidateCollection(OperationContext* opCtx, NamespaceString nss) {
    ValidateResults validateResults;
    Status status =
        CollectionValidation::validate(opCtx,
                                       nss,
                                       {CollectionValidation::ValidateMode::kForegroundFull,
                                        CollectionValidation::RepairMode::kNone,
                                        /*logDiagnostics=*/false},
                                       &validateResults);

    if (!status.isOK()) {
        uassertStatusOK({ErrorCodes::OfflineValidationFailedToComplete,
                         str::stream() << "Validation of collection " << nss.toStringForErrorMsg()
                                       << " failed with error " << status.toString()
                                       << ", see logs for more details"});
    } else {
        BSONObjBuilder results;
        validateResults.appendToResultObj(&results, /*debug=*/false);
        LOGV2_OPTIONS(9437301,
                      {logv2::LogTruncation::Disabled},
                      "Offline validation result",
                      "results"_attr = results.done());
    }
    return validateResults.isValid();
}

// Perform collection validation for all collections on a databases
bool offlineValidateDb(OperationContext* opCtx, DatabaseName dbName) {
    auto databaseHolder = DatabaseHolder::get(opCtx);
    databaseHolder->openDb(opCtx, dbName);
    bool allResultsValid = true;
    if (!gValidateCollectionName.empty()) {
        NamespaceString userNss = NamespaceStringUtil::deserialize(dbName, gValidateCollectionName);
        allResultsValid &= offlineValidateCollection(opCtx, userNss);
    } else {
        for (const auto& nss :
             CollectionCatalog::get(opCtx)->getAllCollectionNamesFromDb(opCtx, dbName)) {
            opCtx->checkForInterrupt();

            allResultsValid &= offlineValidateCollection(opCtx, nss);
        }
    }
    return allResultsValid;
}

// Perform collection validation for all collections in all databases
void offlineValidate(OperationContext* opCtx) {
    invariant(!storageGlobalParams.queryableBackupMode);
    bool allResultsValid = true;

    if (!gValidateDbName.empty()) {
        const boost::optional<TenantId>& tenantId = boost::none;
        auto userDbName = DatabaseNameUtil::deserialize(
            tenantId,
            gValidateDbName.data(),
            SerializationContext(SerializationContext::Source::Catalog));
        allResultsValid &= offlineValidateDb(opCtx, userDbName);

    } else {
        for (const auto& dbName : CollectionCatalog::get(opCtx)->getAllDbNames()) {
            allResultsValid &= offlineValidateDb(opCtx, dbName);
        }
    }

    if (allResultsValid) {
        LOGV2(9437303, "Offline validation successfully validated all collections");
    } else {
        LOGV2(9437304, "Offline validation found issues in some collections, see logs for details");
    }
}

// Perform routine startup recovery procedure.
// The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
// this function into one single builder that records the time elapsed during startup. Its default
// value is nullptr because we only want to time this function when it is called during startup.
void startupRecovery(OperationContext* opCtx,
                     StorageEngine* storageEngine,
                     StorageEngine::LastShutdownState lastShutdownState,
                     BSONObjBuilder* startupTimeElapsedBuilder = nullptr) {
    auto& rss = rss::ReplicatedStorageService::get(opCtx);
    if (rss.getPersistenceProvider().shouldDelayDataAccessDuringStartup()) {
        LOGV2(10985327,
              "Skip startupRecovery; it will be handled later when WT loads the "
              "checkpoint");
        return;
    }

    invariant(!storageGlobalParams.repair);

    ServiceContext* svcCtx = opCtx->getServiceContext();

    // Determine whether this is a replica set node running in standalone mode. This must be set
    // before determining whether to restart index builds.
    setReplSetMemberInStandaloneMode(opCtx, storageEngine);

    // Initialize FCV before rebuilding indexes that may have features dependent on FCV.
    {
        SectionScopedTimer scopedTimer(svcCtx->getFastClockSource(),
                                       TimedSectionId::initializeFCVForIndex,
                                       startupTimeElapsedBuilder);
        FeatureCompatibilityVersion::initializeForStartup(opCtx);
    }

    // Drops abandoned idents. Restarts incomplete two-phase index builds.
    reconcileCatalogAndRestartUnfinishedIndexBuilds(
        opCtx, storageEngine, lastShutdownState, startupTimeElapsedBuilder);

    const bool usingReplication =
        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();

    // On replica set members we only clear temp collections on DBs other than "local" during
    // promotion to primary. On secondaries, they are only cleared when the oplog tells them to. The
    // local DB is special because it is not replicated.  See SERVER-10927 for more details.
    const bool shouldClearNonLocalTmpCollections =
        !(hasReplSetConfigDoc(opCtx) || usingReplication);

    openDatabases(opCtx, [&](const DatabaseName& dbName) {
        // Ensures all collections meet requirements such as having _id indexes, and corrects them
        // if needed.
        uassertStatusOK(
            ensureCollectionProperties(opCtx, dbName, EnsureIndexPolicy::kBuildMissing));

        if (usingReplication) {
            // Ensure oplog is capped (mongodb does not guarantee order of inserts on noncapped
            // collections)
            if (dbName == DatabaseName::kLocal) {
                assertCappedOplog(opCtx);
            }
        }

        if (shouldClearNonLocalTmpCollections || dbName == DatabaseName::kLocal) {
            clearTempCollections(opCtx, dbName);
        }
    });
}

// Returns true if the oplog collection exists. Will always return false if the cached pointer to
// the collection has not yet been initialized.
bool oplogExists(OperationContext* opCtx) {
    return static_cast<bool>(LocalOplogInfo::get(opCtx)->getRecordStore());
}
}  // namespace

namespace startup_recovery {

/**
 * Recovers or repairs all databases from a previous shutdown. May throw a MustDowngrade error
 * if data files are incompatible with the current binary version.
 * The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
 * this function into one single builder that records the time elapsed during startup. Its default
 * value is nullptr because we only want to time this function when it is called during startup.
 */
void repairAndRecoverDatabases(OperationContext* opCtx,
                               StorageEngine::LastShutdownState lastShutdownState,
                               BSONObjBuilder* startupTimeElapsedBuilder) {
    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();

    Lock::GlobalWrite lk(opCtx);

    // Create the FCV document for the first time, if necessary. Replica set nodes only initialize
    // the FCV when the replica set is first initiated or by data replication.
    const bool usingReplication =
        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
    if (isWriteableStorageEngine() && !usingReplication) {
        FeatureCompatibilityVersion::setIfCleanStartup(opCtx, repl::StorageInterface::get(opCtx));
    }

    if (storageGlobalParams.repair) {
        startupRepair(opCtx, storageEngine, startupTimeElapsedBuilder);
    } else if (storageGlobalParams.validate) {
        offlineValidate(opCtx);
    } else {
        startupRecovery(opCtx, storageEngine, lastShutdownState, startupTimeElapsedBuilder);
    }
}

/**
 * Runs startup recovery after system startup, will restart index builds if in replSet mode
 * In no case will it create an FCV document nor run repair or read-only recovery.
 */
void runStartupRecovery(OperationContext* opCtx,
                        StorageEngine::LastShutdownState lastShutdownState) {
    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    Lock::GlobalWrite lk(opCtx);

    invariant(isWriteableStorageEngine());
    invariant(!storageGlobalParams.repair);
    const bool usingReplication =
        repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet();
    invariant(usingReplication);
    startupRecovery(opCtx, storageEngine, lastShutdownState);
}

void recoverChangeStreamCollections(OperationContext* opCtx,
                                    bool isStandalone,
                                    StorageEngine::LastShutdownState lastShutdownState) {
    if (lastShutdownState != StorageEngine::LastShutdownState::kUnclean) {
        // The storage engine guarantees consistent data ranges after truncate on clean
        // shutdown.
        return;
    }

    if (isStandalone && !oplogExists(opCtx)) {
        // If the node is started up as a standalone, it could have either (1) previously been a
        // standalone (the oplog doesn't exist), or (2) previously been a replica set node (the
        // oplog collection exists and the cached pointer to the oplog is initialized, or the oplog
        // collection exists but the cached pointer to the oplog hasn't been initialized yet).
        // If the oplog exists, the pointer has already been cached, even in standalone mode.

        // There is no underlying oplog collection. Nothing to do since change stream collections
        // implicitly replicate from the oplog, and can't exist without it.
        LOGV2_DEBUG(7803704,
                    3,
                    "Skipping truncation of change stream collections on startup recovery "
                    "because there is no oplog");

        return;
    }

    const auto tenantIds = change_stream_serverless_helpers::getConfigDbTenants(opCtx);
    if (tenantIds.empty()) {
        // Change stream pre-images collections are exclusive to dedicated environments.
        cleanupPreImagesCollectionAfterUncleanShutdown(opCtx);
        return;
    }

    for (const auto& tenantId : tenantIds) {
        // Change collections are exclusive to multi-tenant environments.
        cleanupChangeCollectionAfterUncleanShutdown(opCtx, tenantId);
    }
}
}  // namespace startup_recovery
}  // namespace mongo
