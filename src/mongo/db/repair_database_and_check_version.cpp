
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "repair_database_and_check_version.h"

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"

#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace mongo {

// Exit after repair has started, but before data is repaired.
MONGO_FAIL_POINT_DEFINE(exitBeforeDataRepair);
// Exit after repairing data, but before the replica set configuration is invalidated.
MONGO_FAIL_POINT_DEFINE(exitBeforeRepairInvalidatesConfig);

namespace {

const std::string mustDowngradeErrorMsg = str::stream()
    << "UPGRADE PROBLEM: The data files need to be fully upgraded to version 4.0 before attempting "
       "an upgrade to 4.2; see "
    << feature_compatibility_version_documentation::kUpgradeLink << " for more details.";

Status restoreMissingFeatureCompatibilityVersionDocument(OperationContext* opCtx,
                                                         const std::vector<std::string>& dbNames) {
    NamespaceString fcvNss(NamespaceString::kServerConfigurationNamespace);

    // If the admin database, which contains the server configuration collection with the
    // featureCompatibilityVersion document, does not exist, create it.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, fcvNss.db());
    if (!db) {
        log() << "Re-creating admin database that was dropped.";
    }
    db = databaseHolder->openDb(opCtx, fcvNss.db());
    invariant(db);

    // If the server configuration collection, which contains the FCV document, does not exist, then
    // create it.
    if (!db->getCollection(opCtx, NamespaceString::kServerConfigurationNamespace)) {
        log() << "Re-creating the server configuration collection (admin.system.version) that was "
                 "dropped.";
        uassertStatusOK(
            createCollection(opCtx, fcvNss.db().toString(), BSON("create" << fcvNss.coll())));
    }

    Collection* fcvColl = db->getCollection(opCtx, NamespaceString::kServerConfigurationNamespace);
    invariant(fcvColl);

    // Restore the featureCompatibilityVersion document if it is missing.
    BSONObj featureCompatibilityVersion;
    if (!Helpers::findOne(opCtx,
                          fcvColl,
                          BSON("_id" << FeatureCompatibilityVersionParser::kParameterName),
                          featureCompatibilityVersion)) {
        log() << "Re-creating featureCompatibilityVersion document that was deleted with version "
              << FeatureCompatibilityVersionParser::kVersion40 << ".";

        BSONObj fcvObj = BSON("_id" << FeatureCompatibilityVersionParser::kParameterName
                                    << FeatureCompatibilityVersionParser::kVersionField
                                    << FeatureCompatibilityVersionParser::kVersion40);

        writeConflictRetry(opCtx, "insertFCVDocument", fcvNss.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            OpDebug* const nullOpDebug = nullptr;
            uassertStatusOK(
                fcvColl->insertDocument(opCtx, InsertStatement(fcvObj), nullOpDebug, false));
            wunit.commit();
        });
    }

    invariant(Helpers::findOne(opCtx,
                               fcvColl,
                               BSON("_id" << FeatureCompatibilityVersionParser::kParameterName),
                               featureCompatibilityVersion));

    return Status::OK();
}

/**
 * Checks that all replicated collections in the given list of 'dbNames' have UUIDs. Returns a
 * MustDowngrade error status if any do not.
 *
 * Additionally assigns UUIDs to any non-replicated collections that are missing UUIDs.
 */
Status ensureAllCollectionsHaveUUIDs(OperationContext* opCtx,
                                     const std::vector<std::string>& dbNames) {
    auto databaseHolder = DatabaseHolder::get(opCtx);
    for (const auto& dbName : dbNames) {
        auto db = databaseHolder->openDb(opCtx, dbName);
        invariant(db);
        for (auto collectionIt = db->begin(); collectionIt != db->end(); ++collectionIt) {
            Collection* coll = *collectionIt;

            // We expect all collections to have UUIDs in MongoDB 4.2
            if (!coll->uuid()) {
                return {ErrorCodes::MustDowngrade, mustDowngradeErrorMsg};
            }
        }
    }
    return Status::OK();
}

const NamespaceString startupLogCollectionName("local.startup_log");
const NamespaceString kSystemReplSetCollection("local.system.replset");

/**
 * Returns 'true' if this server has a configuration document in local.system.replset.
 */
bool hasReplSetConfigDoc(OperationContext* opCtx) {
    Lock::GlobalWrite lk(opCtx);
    BSONObj config;
    return Helpers::getSingleton(opCtx, kSystemReplSetCollection.ns().c_str(), config);
}

/**
* Check that the oplog is capped, and abort the process if it is not.
* Caller must lock DB before calling this function.
*/
void checkForCappedOplog(OperationContext* opCtx, Database* db) {
    const NamespaceString oplogNss(NamespaceString::kRsOplogNamespace);
    invariant(opCtx->lockState()->isDbLockedForMode(oplogNss.db(), MODE_IS));
    Collection* oplogCollection = db->getCollection(opCtx, oplogNss);
    if (oplogCollection && !oplogCollection->isCapped()) {
        severe() << "The oplog collection " << oplogNss
                 << " is not capped; a capped oplog is a requirement for replication to function.";
        fassertFailedNoTrace(40115);
    }
}

void rebuildIndexes(OperationContext* opCtx, StorageEngine* storageEngine) {
    std::vector<StorageEngine::CollectionIndexNamePair> indexesToRebuild =
        fassert(40593, storageEngine->reconcileCatalogAndIdents(opCtx));

    if (!indexesToRebuild.empty() && serverGlobalParams.indexBuildRetry) {
        log() << "note: restart the server with --noIndexBuildRetry "
              << "to skip index rebuilds";
    }

    if (!serverGlobalParams.indexBuildRetry) {
        log() << "  not rebuilding interrupted indexes";
        return;
    }

    // Determine which indexes need to be rebuilt. rebuildIndexesOnCollection() requires that all
    // indexes on that collection are done at once, so we use a map to group them together.
    StringMap<IndexNameObjs> nsToIndexNameObjMap;
    for (auto&& indexNamespace : indexesToRebuild) {
        NamespaceString collNss(indexNamespace.first);
        const std::string& indexName = indexNamespace.second;

        DatabaseCatalogEntry* dbce = storageEngine->getDatabaseCatalogEntry(opCtx, collNss.db());
        invariant(dbce,
                  str::stream() << "couldn't get database catalog entry for database "
                                << collNss.db());
        CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry(collNss.ns());
        invariant(cce,
                  str::stream() << "couldn't get collection catalog entry for collection "
                                << collNss.toString());

        auto swIndexSpecs = getIndexNameObjs(
            opCtx, dbce, cce, [&indexName](const std::string& name) { return name == indexName; });
        if (!swIndexSpecs.isOK() || swIndexSpecs.getValue().first.empty()) {
            fassert(40590,
                    {ErrorCodes::InternalError,
                     str::stream() << "failed to get index spec for index " << indexName
                                   << " in collection "
                                   << collNss.toString()});
        }

        auto& indexesToRebuild = swIndexSpecs.getValue();
        invariant(indexesToRebuild.first.size() == 1 && indexesToRebuild.second.size() == 1,
                  str::stream() << "Num Index Names: " << indexesToRebuild.first.size()
                                << " Num Index Objects: "
                                << indexesToRebuild.second.size());
        auto& ino = nsToIndexNameObjMap[collNss.ns()];
        ino.first.emplace_back(std::move(indexesToRebuild.first.back()));
        ino.second.emplace_back(std::move(indexesToRebuild.second.back()));
    }

    for (const auto& entry : nsToIndexNameObjMap) {
        NamespaceString collNss(entry.first);

        auto dbCatalogEntry = storageEngine->getDatabaseCatalogEntry(opCtx, collNss.db());
        auto collCatalogEntry = dbCatalogEntry->getCollectionCatalogEntry(collNss.toString());
        for (const auto& indexName : entry.second.first) {
            log() << "Rebuilding index. Collection: " << collNss << " Index: " << indexName;
        }
        fassert(40592,
                rebuildIndexesOnCollection(
                    opCtx, dbCatalogEntry, collCatalogEntry, std::move(entry.second)));
    }
}

}  // namespace

/**
 * Return an error status if the wrong mongod version was used for these datafiles. The boolean
 * represents whether there are non-local databases.
 */
StatusWith<bool> repairDatabasesAndCheckVersion(OperationContext* opCtx) {
    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    Lock::GlobalWrite lk(opCtx);

    std::vector<std::string> dbNames;
    storageEngine->listDatabases(&dbNames);

    // Rebuilding indexes must be done before a database can be opened, except when using repair,
    // which rebuilds all indexes when it is done.
    if (!storageGlobalParams.readOnly && !storageGlobalParams.repair) {
        rebuildIndexes(opCtx, storageEngine);
    }

    bool repairVerifiedAllCollectionsHaveUUIDs = false;

    // Repair all databases first, so that we do not try to open them if they are in bad shape
    auto databaseHolder = DatabaseHolder::get(opCtx);
    if (storageGlobalParams.repair) {
        invariant(!storageGlobalParams.readOnly);

        if (MONGO_FAIL_POINT(exitBeforeDataRepair)) {
            log() << "Exiting because 'exitBeforeDataRepair' fail point was set.";
            quickExit(EXIT_ABRUPT);
        }

        for (const auto& dbName : dbNames) {
            LOG(1) << "    Repairing database: " << dbName;
            fassertNoTrace(18506, repairDatabase(opCtx, storageEngine, dbName));
        }

        // All collections must have UUIDs before restoring the FCV document to a version that
        // requires UUIDs.
        Status uuidsStatus = ensureAllCollectionsHaveUUIDs(opCtx, dbNames);
        if (!uuidsStatus.isOK()) {
            return uuidsStatus;
        }
        repairVerifiedAllCollectionsHaveUUIDs = true;

        // Attempt to restore the featureCompatibilityVersion document if it is missing.
        NamespaceString fcvNSS(NamespaceString::kServerConfigurationNamespace);

        auto db = databaseHolder->getDb(opCtx, fcvNSS.db());
        Collection* versionColl;
        BSONObj featureCompatibilityVersion;
        if (!db || !(versionColl = db->getCollection(opCtx, fcvNSS)) ||
            !Helpers::findOne(opCtx,
                              versionColl,
                              BSON("_id" << FeatureCompatibilityVersionParser::kParameterName),
                              featureCompatibilityVersion)) {
            auto status = restoreMissingFeatureCompatibilityVersionDocument(opCtx, dbNames);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    // All collections must have UUIDs.
    if (!repairVerifiedAllCollectionsHaveUUIDs) {
        Status uuidsStatus = ensureAllCollectionsHaveUUIDs(opCtx, dbNames);
        if (!uuidsStatus.isOK()) {
            return uuidsStatus;
        }
    }

    if (!storageGlobalParams.readOnly) {
        // We open the "local" database before calling hasReplSetConfigDoc() to ensure the in-memory
        // catalog entries for the 'kSystemReplSetCollection' collection have been populated if the
        // collection exists. If the "local" database didn't exist at this point yet, then it will
        // be created. If the mongod is running in a read-only mode, then it is fine to not open the
        // "local" database and populate the catalog entries because we won't attempt to drop the
        // temporary collections anyway.
        Lock::DBLock dbLock(opCtx, kSystemReplSetCollection.db(), MODE_X);
        databaseHolder->openDb(opCtx, kSystemReplSetCollection.db());
    }

    if (storageGlobalParams.repair) {
        if (MONGO_FAIL_POINT(exitBeforeRepairInvalidatesConfig)) {
            log() << "Exiting because 'exitBeforeRepairInvalidatesConfig' fail point was set.";
            quickExit(EXIT_ABRUPT);
        }
        // This must be done after opening the "local" database as it modifies the replica set
        // config.
        auto repairObserver = StorageRepairObserver::get(opCtx->getServiceContext());
        repairObserver->onRepairDone(opCtx);
        if (repairObserver->isDataModified()) {
            warning() << "Modifications made by repair:";
            const auto& mods = repairObserver->getModifications();
            for (const auto& mod : mods) {
                warning() << "  " << mod;
            }
            if (hasReplSetConfigDoc(opCtx)) {
                warning() << "WARNING: Repair may have modified replicated data. This node will no "
                             "longer be able to join a replica set without a full re-sync";
            }
        }
    }

    const repl::ReplSettings& replSettings =
        repl::ReplicationCoordinator::get(opCtx)->getSettings();

    // On replica set members we only clear temp collections on DBs other than "local" during
    // promotion to primary. On pure slaves, they are only cleared when the oplog tells them
    // to. The local DB is special because it is not replicated.  See SERVER-10927 for more
    // details.
    const bool shouldClearNonLocalTmpCollections =
        !(hasReplSetConfigDoc(opCtx) || replSettings.usingReplSets());

    // To check whether a featureCompatibilityVersion document exists.
    bool fcvDocumentExists = false;

    // To check whether we have databases other than local.
    bool nonLocalDatabases = false;

    // Refresh list of database names to include newly-created admin, if it exists.
    dbNames.clear();
    storageEngine->listDatabases(&dbNames);
    for (const auto& dbName : dbNames) {
        if (dbName != "local") {
            nonLocalDatabases = true;
        }
        LOG(1) << "    Recovering database: " << dbName;

        auto db = databaseHolder->openDb(opCtx, dbName);
        invariant(db);

        // First thing after opening the database is to check for file compatibility,
        // otherwise we might crash if this is a deprecated format.
        auto status = db->getDatabaseCatalogEntry()->currentFilesCompatible(opCtx);
        if (!status.isOK()) {
            if (status.code() == ErrorCodes::CanRepairToDowngrade) {
                // Convert CanRepairToDowngrade statuses to MustUpgrade statuses to avoid logging a
                // potentially confusing and inaccurate message.
                //
                // TODO SERVER-24097: Log a message informing the user that they can start the
                // current version of mongod with --repair and then proceed with normal startup.
                status = {ErrorCodes::MustUpgrade, status.reason()};
            }
            severe() << "Unable to start mongod due to an incompatibility with the data files and"
                        " this version of mongod: "
                     << redact(status);
            severe() << "Please consult our documentation when trying to downgrade to a previous"
                        " major release";
            quickExit(EXIT_NEED_UPGRADE);
            MONGO_UNREACHABLE;
        }


        // If the server configuration collection already contains a valid
        // featureCompatibilityVersion document, cache it in-memory as a server parameter.
        if (dbName == "admin") {
            if (Collection* versionColl =
                    db->getCollection(opCtx, NamespaceString::kServerConfigurationNamespace)) {
                BSONObj featureCompatibilityVersion;
                if (Helpers::findOne(
                        opCtx,
                        versionColl,
                        BSON("_id" << FeatureCompatibilityVersionParser::kParameterName),
                        featureCompatibilityVersion)) {
                    auto swVersion =
                        FeatureCompatibilityVersionParser::parse(featureCompatibilityVersion);
                    if (!swVersion.isOK()) {
                        // Note this error path captures all cases of an FCV document existing,
                        // but with any value other than "4.0" or "4.2". This includes unexpected
                        // cases with no path forward such as the FCV value not being a string.
                        return {ErrorCodes::MustDowngrade,
                                str::stream()
                                    << "UPGRADE PROBLEM: Found an invalid "
                                       "featureCompatibilityVersion document (ERROR: "
                                    << swVersion.getStatus()
                                    << "). If the current featureCompatibilityVersion is below "
                                       "4.0, see the documentation on upgrading at "
                                    << feature_compatibility_version_documentation::kUpgradeLink
                                    << "."};
                    }
                    fcvDocumentExists = true;
                    auto version = swVersion.getValue();
                    serverGlobalParams.featureCompatibility.setVersion(version);
                    FeatureCompatibilityVersion::updateMinWireVersion();

                    // On startup, if the version is in an upgrading or downrading state, print a
                    // warning.
                    if (version ==
                        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo42) {
                        log() << "** WARNING: A featureCompatibilityVersion upgrade did not "
                              << "complete. " << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersionParser::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume upgrade to 4.2." << startupWarningsLog;
                    } else if (version == ServerGlobalParams::FeatureCompatibility::Version::
                                              kDowngradingTo40) {
                        log() << "** WARNING: A featureCompatibilityVersion downgrade did not "
                              << "complete. " << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersionParser::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume downgrade to 4.0." << startupWarningsLog;
                    }
                }
            }
        }

        if (replSettings.usingReplSets()) {
            // We only care about _id indexes and drop-pending collections if we are in a replset.
            db->checkForIdIndexesAndDropPendingCollections(opCtx);
            // Ensure oplog is capped (mongodb does not guarantee order of inserts on noncapped
            // collections)
            if (db->name() == "local") {
                checkForCappedOplog(opCtx, db);
            }
        }

        if (!storageGlobalParams.readOnly &&
            (shouldClearNonLocalTmpCollections || dbName == "local")) {
            db->clearTmpCollections(opCtx);
        }
    }

    // Fail to start up if there is no featureCompatibilityVersion document and there are non-local
    // databases present.
    if (!fcvDocumentExists && nonLocalDatabases) {
        severe()
            << "Unable to start up mongod due to missing featureCompatibilityVersion document.";
        severe() << "Please run with --repair to restore the document.";
        fassertFailedNoTrace(40652);
    }

    LOG(1) << "done repairDatabases";
    return nonLocalDatabases;
}

}  // namespace mongo
