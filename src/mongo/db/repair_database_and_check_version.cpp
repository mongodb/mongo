/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "repair_database_and_check_version.h"

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/version.h"

#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace mongo {

using logger::LogComponent;
using std::endl;

namespace {

const std::string mustDowngradeErrorMsg = str::stream()
    << "UPGRADE PROBLEM: The data files need to be fully upgraded to version 3.6 before attempting "
       "an upgrade to 4.0; see "
    << feature_compatibility_version_documentation::kUpgradeLink << " for more details.";

Status restoreMissingFeatureCompatibilityVersionDocument(OperationContext* opCtx,
                                                         const std::vector<std::string>& dbNames) {
    NamespaceString fcvNss(NamespaceString::kServerConfigurationNamespace);

    // If the admin database, which contains the server configuration collection with the
    // featureCompatibilityVersion document, does not exist, create it.
    Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx, fcvNss.db());
    if (!db) {
        log() << "Re-creating admin database that was dropped.";
    }
    db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, fcvNss.db());
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
              << FeatureCompatibilityVersionParser::kVersion36 << ".";

        BSONObj fcvObj = BSON("_id" << FeatureCompatibilityVersionParser::kParameterName
                                    << FeatureCompatibilityVersionParser::kVersionField
                                    << FeatureCompatibilityVersionParser::kVersion36);

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
    bool isMmapV1 = opCtx->getServiceContext()->getStorageEngine()->isMmapV1();
    for (const auto& dbName : dbNames) {
        Database* db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, dbName);
        invariant(db);
        for (auto collectionIt = db->begin(); collectionIt != db->end(); ++collectionIt) {
            Collection* coll = *collectionIt;
            // The presence of system.indexes or system.namespaces on wiredTiger may
            // have undesirable results (see SERVER-32894, SERVER-34482). It is okay to
            // drop these collections on wiredTiger because users are not permitted to
            // store data in them.
            if (coll->ns().coll() == "system.indexes" || coll->ns().coll() == "system.namespaces") {
                if (isMmapV1) {
                    // system.indexes and system.namespaces don't currently have UUIDs in MMAP.
                    // SERVER-29926 and SERVER-30095 will address this problem.
                    continue;
                }
                const auto nssToDrop = coll->ns();
                LOG(1) << "Attempting to drop invalid system collection " << nssToDrop;
                if (coll->numRecords(opCtx)) {
                    severe(LogComponent::kControl) << "Cannot drop non-empty collection "
                                                   << nssToDrop.ns();
                    exitCleanly(EXIT_NEED_DOWNGRADE);
                }
                repl::UnreplicatedWritesBlock uwb(opCtx);
                writeConflictRetry(opCtx, "dropSystemIndexes", nssToDrop.ns(), [&] {
                    WriteUnitOfWork wunit(opCtx);
                    BSONObjBuilder unusedResult;
                    fassert(50837,
                            dropCollection(
                                opCtx,
                                nssToDrop,
                                unusedResult,
                                {},
                                DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops));
                    wunit.commit();
                });
                continue;
            }

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
 * Checks if this server was started without --replset but has a config in local.system.replset
 * (meaning that this is probably a replica set member started in stand-alone mode).
 *
 * @returns the number of documents in local.system.replset or 0 if this was started with
 *          --replset.
 */
unsigned long long checkIfReplMissingFromCommandLine(OperationContext* opCtx) {
    // This is helpful for the query below to work as you can't open files when readlocked
    Lock::GlobalWrite lk(opCtx);
    if (!repl::ReplicationCoordinator::get(getGlobalServiceContext())
             ->getSettings()
             .usingReplSets()) {
        DBDirectClient c(opCtx);
        return c.count(kSystemReplSetCollection.ns());
    }
    return 0;
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
    LOG(1) << "enter repairDatabases (to check pdfile version #)";

    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();

    Lock::GlobalWrite lk(opCtx);

    std::vector<std::string> dbNames;
    storageEngine->listDatabases(&dbNames);

    // Rebuilding indexes must be done before a database can be opened.
    if (!storageGlobalParams.readOnly) {
        rebuildIndexes(opCtx, storageEngine);
    }

    bool repairVerifiedAllCollectionsHaveUUIDs = false;

    // Repair all databases first, so that we do not try to open them if they are in bad shape
    if (storageGlobalParams.repair) {
        invariant(!storageGlobalParams.readOnly);
        for (const auto& dbName : dbNames) {
            LOG(1) << "    Repairing database: " << dbName;
            fassert(18506, repairDatabase(opCtx, storageEngine, dbName));
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

        Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx, fcvNSS.db());
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
        // We open the "local" database before calling checkIfReplMissingFromCommandLine() to
        // ensure the in-memory catalog entries for the 'kSystemReplSetCollection' collection have
        // been populated if the collection exists. If the "local" database didn't exist at this
        // point yet, then it will be created. If the mongod is running in a read-only mode, then
        // it is fine to not open the "local" database and populate the catalog entries because we
        // won't attempt to drop the temporary collections anyway.
        Lock::DBLock dbLock(opCtx, kSystemReplSetCollection.db(), MODE_X);
        DatabaseHolder::getDatabaseHolder().openDb(opCtx, kSystemReplSetCollection.db());
    }

    const repl::ReplSettings& replSettings =
        repl::ReplicationCoordinator::get(opCtx)->getSettings();

    // On replica set members we only clear temp collections on DBs other than "local" during
    // promotion to primary. On pure slaves, they are only cleared when the oplog tells them
    // to. The local DB is special because it is not replicated.  See SERVER-10927 for more
    // details.
    const bool shouldClearNonLocalTmpCollections =
        !(checkIfReplMissingFromCommandLine(opCtx) || replSettings.usingReplSets());

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

        Database* db = DatabaseHolder::getDatabaseHolder().openDb(opCtx, dbName);
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
                        severe() << swVersion.getStatus();
                        // Note this error path captures all cases of an FCV document existing,
                        // but with any value other than "3.6" or "4.0". This includes unexpected
                        // cases with no path forward such as the FCV value not being a string.
                        return {ErrorCodes::MustDowngrade,
                                str::stream()
                                    << "UPGRADE PROBLEM: Unable to parse the "
                                       "featureCompatibilityVersion document. The data files need "
                                       "to be fully upgraded to version 3.6 before attempting an "
                                       "upgrade to 4.0. If you are upgrading to 4.0, see "
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
                        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40) {
                        log() << "** WARNING: A featureCompatibilityVersion upgrade did not "
                              << "complete. " << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersionParser::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume upgrade to 4.0." << startupWarningsLog;
                    } else if (version == ServerGlobalParams::FeatureCompatibility::Version::
                                              kDowngradingTo36) {
                        log() << "** WARNING: A featureCompatibilityVersion downgrade did not "
                              << "complete. " << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersionParser::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume downgrade to 3.6." << startupWarningsLog;
                    }
                }
            }
        }

        // Major versions match, check indexes
        const NamespaceString systemIndexes(db->name(), "system.indexes");

        Collection* coll = db->getCollection(opCtx, systemIndexes);
        auto exec = InternalPlanner::collectionScan(
            opCtx, systemIndexes.ns(), coll, PlanExecutor::NO_YIELD);

        BSONObj index;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&index, NULL))) {
            const BSONObj key = index.getObjectField("key");
            const auto plugin = IndexNames::findPluginName(key);

            if (db->getDatabaseCatalogEntry()->isOlderThan24(opCtx)) {
                if (IndexNames::existedBefore24(plugin)) {
                    continue;
                }

                log() << "Index " << index << " claims to be of type '" << plugin << "', "
                      << "which is either invalid or did not exist before v2.4. "
                      << "See the upgrade section: "
                      << "http://dochub.mongodb.org/core/upgrade-2.4" << startupWarningsLog;
            }

            if (index["v"].isNumber() && index["v"].numberInt() == 0) {
                log() << "WARNING: The index: " << index << " was created with the deprecated"
                      << " v:0 format.  This format will not be supported in a future release."
                      << startupWarningsLog;
                log() << "\t To fix this, you need to rebuild this index."
                      << " For instructions, see http://dochub.mongodb.org/core/rebuild-v0-indexes"
                      << startupWarningsLog;
            }
        }

        // Non-yielding collection scans from InternalPlanner will never error.
        invariant(PlanExecutor::IS_EOF == state);

        if (replSettings.usingReplSets()) {
            // We only care about _id indexes and drop-pending collections if we are in a replset.
            checkForIdIndexesAndDropPendingCollections(opCtx, db);
            // Ensure oplog is capped (mmap does not guarantee order of inserts on noncapped
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
        if (opCtx->getServiceContext()->getStorageEngine()->isMmapV1()) {
            severe() << "Please run with --journalOptions "
                     << static_cast<int>(MMAPV1Options::JournalRecoverOnly)
                     << " to recover the journal. Then run with --repair to restore the document.";
        } else {
            severe() << "Please run with --repair to restore the document.";
        }
        fassertFailedNoTrace(40652);
    }

    LOG(1) << "done repairDatabases";
    return nonLocalDatabases;
}

/**
 * If we are in a replset, every replicated collection must have an _id index.
 * As we scan each database, we also gather a list of drop-pending collection namespaces for
 * the DropPendingCollectionReaper to clean up eventually.
 */
void checkForIdIndexesAndDropPendingCollections(OperationContext* opCtx, Database* db) {
    if (db->name() == "local") {
        // Collections in the local database are not replicated, so we do not need an _id index on
        // any collection. For the same reason, it is not possible for the local database to contain
        // any drop-pending collections (drops are effective immediately).
        return;
    }

    std::list<std::string> collectionNames;
    db->getDatabaseCatalogEntry()->getCollectionNamespaces(&collectionNames);

    for (const auto& collectionName : collectionNames) {
        const NamespaceString ns(collectionName);

        if (ns.isDropPendingNamespace()) {
            auto dropOpTime = fassert(40459, ns.getDropPendingNamespaceOpTime());
            log() << "Found drop-pending namespace " << ns << " with drop optime " << dropOpTime;
            repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(dropOpTime, ns);
        }

        if (ns.isSystem())
            continue;

        Collection* coll = db->getCollection(opCtx, collectionName);
        if (!coll)
            continue;

        if (coll->getIndexCatalog()->findIdIndex(opCtx))
            continue;

        log() << "WARNING: the collection '" << collectionName << "' lacks a unique index on _id."
              << " This index is needed for replication to function properly" << startupWarningsLog;
        log() << "\t To fix this, you need to create a unique index on _id."
              << " See http://dochub.mongodb.org/core/build-replica-set-indexes"
              << startupWarningsLog;
    }
}

}  // namespace mongo
