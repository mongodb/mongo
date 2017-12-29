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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
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

constexpr StringData upgradeLink = "http://dochub.mongodb.org/core/3.6-upgrade-fcv"_sd;
constexpr StringData mustDowngradeErrorMsg =
    "UPGRADE PROBLEM: The data files need to be fully upgraded to version 3.4 before attempting an upgrade to 3.6; see http://dochub.mongodb.org/core/3.6-upgrade-fcv for more details."_sd;

Status restoreMissingFeatureCompatibilityVersionDocument(OperationContext* opCtx,
                                                         const std::vector<std::string>& dbNames) {
    bool isMmapV1 = opCtx->getServiceContext()->getGlobalStorageEngine()->isMmapV1();
    // Check whether there are any collections with UUIDs.
    bool collsHaveUuids = false;
    bool allCollsHaveUuids = true;
    for (const auto& dbName : dbNames) {
        Database* db = dbHolder().openDb(opCtx, dbName);
        invariant(db);
        for (auto collectionIt = db->begin(); collectionIt != db->end(); ++collectionIt) {
            Collection* coll = *collectionIt;
            if (coll->uuid()) {
                collsHaveUuids = true;
            } else if (!coll->uuid() && (!isMmapV1 ||
                                         !(coll->ns().coll() == "system.indexes" ||
                                           coll->ns().coll() == "system.namespaces"))) {
                // Exclude system.indexes and system.namespaces from the UUID check until
                // SERVER-29926 and SERVER-30095 are addressed, respectively.
                allCollsHaveUuids = false;
            }
        }
    }

    if (!collsHaveUuids) {
        return {ErrorCodes::MustDowngrade, mustDowngradeErrorMsg};
    }

    // Restore the featureCompatibilityVersion document if it is missing.
    NamespaceString nss(FeatureCompatibilityVersion::kCollection);

    Database* db = dbHolder().get(opCtx, nss.db());

    // If the admin database does not exist, create it.
    if (!db) {
        log() << "Re-creating admin database that was dropped.";
    }

    db = dbHolder().openDb(opCtx, nss.db());
    invariant(db);

    // If admin.system.version does not exist, create it without a UUID.
    if (!db->getCollection(opCtx, FeatureCompatibilityVersion::kCollection)) {
        log() << "Re-creating admin.system.version collection that was dropped.";
        allCollsHaveUuids = false;
        BSONObjBuilder bob;
        bob.append("create", nss.coll());
        BSONObj createObj = bob.done();
        uassertStatusOK(createCollection(opCtx, nss.db().toString(), createObj));
    }

    Collection* versionColl = db->getCollection(opCtx, FeatureCompatibilityVersion::kCollection);
    invariant(versionColl);
    BSONObj featureCompatibilityVersion;
    if (!Helpers::findOne(opCtx,
                          versionColl,
                          BSON("_id" << FeatureCompatibilityVersion::kParameterName),
                          featureCompatibilityVersion)) {
        log() << "Re-creating featureCompatibilityVersion document that was deleted.";
        BSONObjBuilder bob;
        bob.append("_id", FeatureCompatibilityVersion::kParameterName);
        if (allCollsHaveUuids) {
            // If all collections have UUIDs, create a featureCompatibilityVersion document with
            // version equal to 3.6.
            bob.append(FeatureCompatibilityVersion::kVersionField,
                       FeatureCompatibilityVersionCommandParser::kVersion36);
        } else {
            // If some collections do not have UUIDs, create a featureCompatibilityVersion document
            // with version equal to 3.4 and targetVersion 3.6.
            bob.append(FeatureCompatibilityVersion::kVersionField,
                       FeatureCompatibilityVersionCommandParser::kVersion34);
            bob.append(FeatureCompatibilityVersion::kTargetVersionField,
                       FeatureCompatibilityVersionCommandParser::kVersion36);
        }
        auto fcvObj = bob.done();
        writeConflictRetry(opCtx, "insertFCVDocument", nss.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            OpDebug* const nullOpDebug = nullptr;
            uassertStatusOK(
                versionColl->insertDocument(opCtx, InsertStatement(fcvObj), nullOpDebug, false));
            wunit.commit();
        });
    }
    invariant(Helpers::findOne(opCtx,
                               versionColl,
                               BSON("_id" << FeatureCompatibilityVersion::kParameterName),
                               featureCompatibilityVersion));
    return Status::OK();
}

const NamespaceString startupLogCollectionName("local.startup_log");
const NamespaceString kSystemReplSetCollection("local.system.replset");

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
            auto dropOpTime = fassertStatusOK(40459, ns.getDropPendingNamespaceOpTime());
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
    if (!repl::getGlobalReplicationCoordinator()->getSettings().usingReplSets()) {
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
}  // namespace

/**
* Return an error status if the wrong mongod version was used for these datafiles. The boolean
* represents whether there are non-local databases.
*/
StatusWith<bool> repairDatabasesAndCheckVersion(OperationContext* opCtx) {
    LOG(1) << "enter repairDatabases (to check pdfile version #)";

    auto const storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();

    Lock::GlobalWrite lk(opCtx);

    std::vector<std::string> dbNames;
    storageEngine->listDatabases(&dbNames);

    // Repair all databases first, so that we do not try to open them if they are in bad shape
    if (storageGlobalParams.repair) {
        invariant(!storageGlobalParams.readOnly);
        for (const auto& dbName : dbNames) {
            LOG(1) << "    Repairing database: " << dbName;
            fassert(18506, repairDatabase(opCtx, storageEngine, dbName));
        }

        // Attempt to restore the featureCompatibilityVersion document if it is missing.
        NamespaceString nss(FeatureCompatibilityVersion::kCollection);

        Database* db = dbHolder().get(opCtx, nss.db());
        Collection* versionColl;
        BSONObj featureCompatibilityVersion;
        if (!db || !(versionColl = db->getCollection(opCtx, nss)) ||
            !Helpers::findOne(opCtx,
                              versionColl,
                              BSON("_id" << FeatureCompatibilityVersion::kParameterName),
                              featureCompatibilityVersion)) {
            auto status = restoreMissingFeatureCompatibilityVersionDocument(opCtx, dbNames);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    const repl::ReplSettings& replSettings =
        repl::ReplicationCoordinator::get(opCtx)->getSettings();

    if (!storageGlobalParams.readOnly) {
        StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>> swIndexesToRebuild =
            storageEngine->reconcileCatalogAndIdents(opCtx);
        fassertStatusOK(40593, swIndexesToRebuild);
        for (auto&& collIndexPair : swIndexesToRebuild.getValue()) {
            const std::string& coll = collIndexPair.first;
            const std::string& indexName = collIndexPair.second;
            DatabaseCatalogEntry* dbce =
                storageEngine->getDatabaseCatalogEntry(opCtx, NamespaceString(coll).db());
            invariant(dbce);
            CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry(coll);
            invariant(cce);

            StatusWith<IndexNameObjs> swIndexToRebuild(
                getIndexNameObjs(opCtx, dbce, cce, [&indexName](const std::string& str) {
                    return str == indexName;
                }));
            if (!swIndexToRebuild.isOK() || swIndexToRebuild.getValue().first.empty()) {
                severe() << "Unable to get indexes for collection. Collection: " << coll;
                fassertFailedNoTrace(40590);
            }

            invariant(swIndexToRebuild.getValue().first.size() == 1 &&
                      swIndexToRebuild.getValue().second.size() == 1);
            fassertStatusOK(
                40592, rebuildIndexesOnCollection(opCtx, dbce, cce, swIndexToRebuild.getValue()));
        }

        // We open the "local" database before calling checkIfReplMissingFromCommandLine() to
        // ensure the in-memory catalog entries for the 'kSystemReplSetCollection' collection have
        // been populated if the collection exists. If the "local" database didn't exist at this
        // point yet, then it will be created. If the mongod is running in a read-only mode, then
        // it is fine to not open the "local" database and populate the catalog entries because we
        // won't attempt to drop the temporary collections anyway.
        Lock::DBLock dbLock(opCtx, kSystemReplSetCollection.db(), MODE_X);
        dbHolder().openDb(opCtx, kSystemReplSetCollection.db());
    }

    // On replica set members we only clear temp collections on DBs other than "local" during
    // promotion to primary. On pure slaves, they are only cleared when the oplog tells them
    // to. The local DB is special because it is not replicated.  See SERVER-10927 for more
    // details.
    const bool shouldClearNonLocalTmpCollections =
        !(checkIfReplMissingFromCommandLine(opCtx) || replSettings.usingReplSets() ||
          replSettings.isSlave());

    // To check whether we are upgrading to 3.6 or have already upgraded to 3.6.
    bool collsHaveUuids = false;

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

        Database* db = dbHolder().openDb(opCtx, dbName);
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

        // Check if admin.system.version contains an invalid featureCompatibilityVersion.
        // If a valid featureCompatibilityVersion is present, cache it as a server parameter.
        if (dbName == "admin") {
            if (Collection* versionColl =
                    db->getCollection(opCtx, FeatureCompatibilityVersion::kCollection)) {
                BSONObj featureCompatibilityVersion;
                if (Helpers::findOne(opCtx,
                                     versionColl,
                                     BSON("_id" << FeatureCompatibilityVersion::kParameterName),
                                     featureCompatibilityVersion)) {
                    auto swVersion =
                        FeatureCompatibilityVersion::parse(featureCompatibilityVersion);
                    if (!swVersion.isOK()) {
                        severe() << swVersion.getStatus();
                        // Note this error path captures all cases of an FCV document existing,
                        // but with any value other than "3.4" or "3.6". This includes unexpected
                        // cases with no path forward such as the FCV value not being a string.
                        return {ErrorCodes::MustDowngrade,
                                str::stream()
                                    << "UPGRADE PROBLEM: Unable to parse the "
                                       "featureCompatibilityVersion document. The data files need "
                                       "to be fully upgraded to version 3.4 before attempting an "
                                       "upgrade to 3.6. If you are upgrading to 3.6, see "
                                    << upgradeLink
                                    << "."};
                    }
                    fcvDocumentExists = true;
                    auto version = swVersion.getValue();
                    serverGlobalParams.featureCompatibility.setVersion(version);
                    FeatureCompatibilityVersion::updateMinWireVersion();

                    // On startup, if the version is in an upgrading or downrading state, print a
                    // warning.
                    if (version ==
                        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36) {
                        log() << "** WARNING: A featureCompatibilityVersion upgrade did not "
                              << "complete." << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersion::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume upgrade to 3.6." << startupWarningsLog;
                    } else if (version == ServerGlobalParams::FeatureCompatibility::Version::
                                              kDowngradingTo34) {
                        log() << "** WARNING: A featureCompatibilityVersion downgrade did not "
                              << "complete. " << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersion::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume downgrade to 3.4." << startupWarningsLog;
                    } else if (version ==
                               ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40) {
                        log() << "** WARNING: A featureCompatibilityVersion upgrade did not "
                              << "complete. " << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersion::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume upgrade to 4.0." << startupWarningsLog;
                    } else if (version == ServerGlobalParams::FeatureCompatibility::Version::
                                              kDowngradingTo36) {
                        log() << "** WARNING: A featureCompatibilityVersion downgrade did not "
                              << "complete. " << startupWarningsLog;
                        log() << "**          The current featureCompatibilityVersion is "
                              << FeatureCompatibilityVersion::toString(version) << "."
                              << startupWarningsLog;
                        log() << "**          To fix this, use the setFeatureCompatibilityVersion "
                              << "command to resume downgrade to 3.6." << startupWarningsLog;
                    }
                }
            }
        }

        // Iterate through collections and check for UUIDs.
        for (auto collectionIt = db->begin(); !collsHaveUuids && collectionIt != db->end();
             ++collectionIt) {
            Collection* coll = *collectionIt;
            if (coll->uuid()) {
                collsHaveUuids = true;
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
        if (collsHaveUuids) {
            severe()
                << "Unable to start up mongod due to missing featureCompatibilityVersion document.";
            if (opCtx->getServiceContext()->getGlobalStorageEngine()->isMmapV1()) {
                severe()
                    << "Please run with --journalOptions "
                    << static_cast<int>(MMAPV1Options::JournalRecoverOnly)
                    << " to recover the journal. Then run with --repair to restore the document.";
            } else {
                severe() << "Please run with --repair to restore the document.";
            }
            fassertFailedNoTrace(40652);
        } else {
            return {ErrorCodes::MustDowngrade, mustDowngradeErrorMsg};
        }
    }

    LOG(1) << "done repairDatabases";
    return nonLocalDatabases;
}
}  // namespace mongo
