/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/dbmain.h"

#include <boost/filesystem/operations.hpp>
#include <boost/optional.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <signal.h>
#include <string>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/generic_cursor_manager.h"
#include "mongo/db/generic_cursor_manager_mongod.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/initialize_snmp.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/keys_collection_manager_sharding.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_cache_factory_mongod.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_killer.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/system_index.h"
#include "mongo/db/ttl.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/process_id.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"
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

#ifdef _WIN32
const ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoDB", L"MongoDB", L"MongoDB Server"};
#endif

void logStartup(OperationContext* opCtx) {
    BSONObjBuilder toLog;
    std::stringstream id;
    id << getHostNameCached() << "-" << jsTime().asInt64();
    toLog.append("_id", id.str());
    toLog.append("hostname", getHostNameCached());

    toLog.appendTimeT("startTime", time(0));
    toLog.append("startTimeLocal", dateToCtimeString(Date_t::now()));

    toLog.append("cmdLine", serverGlobalParams.parsedOpts);
    toLog.append("pid", ProcessId::getCurrent().asLongLong());


    BSONObjBuilder buildinfo(toLog.subobjStart("buildinfo"));
    VersionInfoInterface::instance().appendBuildInfo(&buildinfo);
    appendStorageEngineList(&buildinfo);
    buildinfo.doneFast();

    BSONObj o = toLog.obj();

    Lock::GlobalWrite lk(opCtx);
    AutoGetOrCreateDb autoDb(opCtx, startupLogCollectionName.db(), mongo::MODE_X);
    Database* db = autoDb.getDb();
    Collection* collection = db->getCollection(opCtx, startupLogCollectionName);
    WriteUnitOfWork wunit(opCtx);
    if (!collection) {
        BSONObj options = BSON("capped" << true << "size" << 10 * 1024 * 1024);
        repl::UnreplicatedWritesBlock uwb(opCtx);
        uassertStatusOK(userCreateNS(opCtx, db, startupLogCollectionName.ns(), options));
        collection = db->getCollection(opCtx, startupLogCollectionName);
    }
    invariant(collection);

    OpDebug* const nullOpDebug = nullptr;
    uassertStatusOK(collection->insertDocument(opCtx, InsertStatement(o), nullOpDebug, false));
    wunit.commit();
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

    const repl::ReplSettings& replSettings = repl::getGlobalReplicationCoordinator()->getSettings();

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

void initWireSpec() {
    WireSpec& spec = WireSpec::instance();

    // The featureCompatibilityVersion behavior defaults to the downgrade behavior while the
    // in-memory version is unset.

    spec.incomingInternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;

    spec.outgoing.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;

    spec.isInternalClient = true;
}

MONGO_FP_DECLARE(shutdownAtStartup);

ExitCode _initAndListen(int listenPort) {
    Client::initThread("initandlisten");

    initWireSpec();
    auto serviceContext = checked_cast<ServiceContextMongoD*>(getGlobalServiceContext());

    serviceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds(10)));
    serviceContext->setOpObserver(stdx::make_unique<OpObserverImpl>());

    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    const repl::ReplSettings& replSettings =
        repl::ReplicationCoordinator::get(serviceContext)->getSettings();

    {
        ProcessId pid = ProcessId::getCurrent();
        LogstreamBuilder l = log(LogComponent::kControl);
        l << "MongoDB starting : pid=" << pid << " port=" << serverGlobalParams.port
          << " dbpath=" << storageGlobalParams.dbpath;
        if (replSettings.isMaster())
            l << " master=" << replSettings.isMaster();
        if (replSettings.isSlave())
            l << " slave=" << (int)replSettings.isSlave();

        const bool is32bit = sizeof(int*) == 4;
        l << (is32bit ? " 32" : " 64") << "-bit host=" << getHostNameCached() << endl;
    }

    DEV log(LogComponent::kControl) << "DEBUG build (which is slower)" << endl;

#if defined(_WIN32)
    VersionInfoInterface::instance().logTargetMinOS();
#endif

    logProcessDetails();

    serviceContext->createLockFile();

    serviceContext->setServiceEntryPoint(
        stdx::make_unique<ServiceEntryPointMongod>(serviceContext));

    {
        auto tl =
            transport::TransportLayerManager::createWithConfig(&serverGlobalParams, serviceContext);
        auto res = tl->setup();
        if (!res.isOK()) {
            error() << "Failed to set up listener: " << res;
            return EXIT_NET_ERROR;
        }
        serviceContext->setTransportLayer(std::move(tl));
    }

    serviceContext->initializeGlobalStorageEngine();

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
    if (EncryptionHooks::get(serviceContext)->restartRequired()) {
        exitCleanly(EXIT_CLEAN);
    }
#endif

    // Warn if we detect configurations for multiple registered storage engines in the same
    // configuration file/environment.
    if (serverGlobalParams.parsedOpts.hasField("storage")) {
        BSONElement storageElement = serverGlobalParams.parsedOpts.getField("storage");
        invariant(storageElement.isABSONObj());
        BSONObj storageParamsObj = storageElement.Obj();
        BSONObjIterator i = storageParamsObj.begin();
        while (i.more()) {
            BSONElement e = i.next();
            // Ignore if field name under "storage" matches current storage engine.
            if (storageGlobalParams.engine == e.fieldName()) {
                continue;
            }

            // Warn if field name matches non-active registered storage engine.
            if (serviceContext->isRegisteredStorageEngine(e.fieldName())) {
                warning() << "Detected configuration for non-active storage engine "
                          << e.fieldName() << " when current storage engine is "
                          << storageGlobalParams.engine;
            }
        }
    }

    logMongodStartupWarnings(storageGlobalParams, serverGlobalParams, serviceContext);

    {
        std::stringstream ss;
        ss << endl;
        ss << "*********************************************************************" << endl;
        ss << " ERROR: dbpath (" << storageGlobalParams.dbpath << ") does not exist." << endl;
        ss << " Create this directory or give existing directory in --dbpath." << endl;
        ss << " See http://dochub.mongodb.org/core/startingandstoppingmongo" << endl;
        ss << "*********************************************************************" << endl;
        uassert(10296, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.dbpath));
    }

    {
        std::stringstream ss;
        ss << "repairpath (" << storageGlobalParams.repairpath << ") does not exist";
        uassert(12590, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.repairpath));
    }

    initializeSNMP();

    if (!storageGlobalParams.readOnly) {
        boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");
    }

    if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalRecoverOnly)
        return EXIT_NET_ERROR;

    if (mongodGlobalParams.scriptingEnabled) {
        ScriptEngine::setup();
    }

    auto startupOpCtx = serviceContext->makeOperationContext(&cc());

    bool canCallFCVSetIfCleanStartup = !storageGlobalParams.readOnly &&
        !(replSettings.isSlave() || storageGlobalParams.engine == "devnull");
    if (canCallFCVSetIfCleanStartup && !replSettings.usingReplSets()) {
        Lock::GlobalWrite lk(startupOpCtx.get());
        FeatureCompatibilityVersion::setIfCleanStartup(startupOpCtx.get(),
                                                       repl::StorageInterface::get(serviceContext));
    }

    auto swNonLocalDatabases = repairDatabasesAndCheckVersion(startupOpCtx.get());
    if (!swNonLocalDatabases.isOK()) {
        // SERVER-31611 introduced a return value to `repairDatabasesAndCheckVersion`. Previously,
        // a failing condition would fassert. SERVER-31611 covers a case where the binary (3.6) is
        // refusing to start up because it refuses acknowledgement of FCV 3.2 and requires the
        // user to start up with an older binary. Thus shutting down the server must leave the
        // datafiles in a state that the older binary can start up. This requires going through a
        // clean shutdown.
        //
        // The invariant is *not* a statement that `repairDatabasesAndCheckVersion` must return
        // `MustDowngrade`. Instead, it is meant as a guardrail to protect future developers from
        // accidentally buying into this behavior. New errors that are returned from the method
        // may or may not want to go through a clean shutdown, and they likely won't want the
        // program to return an exit code of `EXIT_NEED_DOWNGRADE`.
        severe(LogComponent::kControl) << "** IMPORTANT: "
                                       << swNonLocalDatabases.getStatus().reason();
        invariant(swNonLocalDatabases == ErrorCodes::MustDowngrade);
        exitCleanly(EXIT_NEED_DOWNGRADE);
    }

    // Assert that the in-memory featureCompatibilityVersion parameter has been explicitly set. If
    // we are part of a replica set and are started up with no data files, we do not set the
    // featureCompatibilityVersion until a primary is chosen. For this case, we expect the in-memory
    // featureCompatibilityVersion parameter to still be uninitialized until after startup.
    if (canCallFCVSetIfCleanStartup &&
        (!replSettings.usingReplSets() || swNonLocalDatabases.getValue())) {
        invariant(serverGlobalParams.featureCompatibility.isVersionInitialized());
    }

    if (storageGlobalParams.upgrade) {
        log() << "finished checking dbs";
        exitCleanly(EXIT_CLEAN);
    }

    // Start up health log writer thread.
    HealthLog::get(startupOpCtx.get()).startup();

    auto const globalAuthzManager = AuthorizationManager::get(serviceContext);
    uassertStatusOK(globalAuthzManager->initialize(startupOpCtx.get()));

    // This is for security on certain platforms (nonce generation)
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));

    if (globalAuthzManager->shouldValidateAuthSchemaOnStartup()) {
        Status status = verifySystemIndexes(startupOpCtx.get());
        if (!status.isOK()) {
            log() << redact(status);
            if (status == ErrorCodes::AuthSchemaIncompatible) {
                exitCleanly(EXIT_NEED_UPGRADE);
            } else {
                quickExit(EXIT_FAILURE);
            }
        }

        // SERVER-14090: Verify that auth schema version is schemaVersion26Final.
        int foundSchemaVersion;
        status =
            globalAuthzManager->getAuthorizationVersion(startupOpCtx.get(), &foundSchemaVersion);
        if (!status.isOK()) {
            log() << "Auth schema version is incompatible: "
                  << "User and role management commands require auth data to have "
                  << "at least schema version " << AuthorizationManager::schemaVersion26Final
                  << " but startup could not verify schema version: " << status;
            exitCleanly(EXIT_NEED_UPGRADE);
        }

        if (foundSchemaVersion < AuthorizationManager::schemaVersion26Final) {
            log() << "Auth schema version is incompatible: "
                  << "User and role management commands require auth data to have "
                  << "at least schema version " << AuthorizationManager::schemaVersion26Final
                  << " but found " << foundSchemaVersion << ". In order to upgrade "
                  << "the auth schema, first downgrade MongoDB binaries to version "
                  << "2.6 and then run the authSchemaUpgrade command.";
            exitCleanly(EXIT_NEED_UPGRADE);
        }

        if (foundSchemaVersion <= AuthorizationManager::schemaVersion26Final) {
            log() << startupWarningsLog;
            log() << "** WARNING: This server is using MONGODB-CR, a deprecated authentication "
                  << "mechanism." << startupWarningsLog;
            log() << "**          Support will be dropped in a future release."
                  << startupWarningsLog;
            log() << "**          See http://dochub.mongodb.org/core/3.0-upgrade-to-scram-sha-1"
                  << startupWarningsLog;
        }
    } else if (globalAuthzManager->isAuthEnabled()) {
        error() << "Auth must be disabled when starting without auth schema validation";
        exitCleanly(EXIT_BADOPTIONS);
    } else {
        // If authSchemaValidation is disabled and server is running without auth,
        // warn the user and continue startup without authSchema metadata checks.
        log() << startupWarningsLog;
        log() << "** WARNING: Startup auth schema validation checks are disabled for the "
                 "database."
              << startupWarningsLog;
        log() << "**          This mode should only be used to manually repair corrupted auth "
                 "data."
              << startupWarningsLog;
    }

    SessionCatalog::create(serviceContext);

    // This function may take the global lock.
    auto shardingInitialized =
        uassertStatusOK(ShardingState::get(startupOpCtx.get())
                            ->initializeShardingAwarenessIfNeeded(startupOpCtx.get()));
    if (shardingInitialized) {
        waitForShardRegistryReload(startupOpCtx.get()).transitional_ignore();
    }

    if (!storageGlobalParams.readOnly) {
        logStartup(startupOpCtx.get());

        startMongoDFTDC();

        restartInProgressIndexesFromLastShutdown(startupOpCtx.get());

        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            // Note: For replica sets, ShardingStateRecovery happens on transition to primary.
            if (!repl::getGlobalReplicationCoordinator()->isReplEnabled()) {
                uassertStatusOK(ShardingStateRecovery::recover(startupOpCtx.get()));
            }
        } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            ShardedConnectionInfo::addHook(startupOpCtx->getServiceContext());

            uassertStatusOK(
                initializeGlobalShardingStateForMongod(startupOpCtx.get(),
                                                       ConnectionString::forLocal(),
                                                       kDistLockProcessIdForConfigServer));

            Balancer::create(startupOpCtx->getServiceContext());

            ShardingCatalogManager::create(
                startupOpCtx->getServiceContext(),
                makeShardingTaskExecutor(executor::makeNetworkInterface("AddShard-TaskExecutor")));
        } else if (replSettings.usingReplSets()) {  // standalone replica set
            auto keysCollectionClient = stdx::make_unique<KeysCollectionClientDirect>();
            auto keyManager = std::make_shared<KeysCollectionManagerSharding>(
                KeysCollectionManager::kKeyManagerPurposeString,
                std::move(keysCollectionClient),
                Seconds(KeysRotationIntervalSec));
            keyManager->startMonitoring(startupOpCtx->getServiceContext());

            LogicalTimeValidator::set(startupOpCtx->getServiceContext(),
                                      stdx::make_unique<LogicalTimeValidator>(keyManager));
        }

        repl::ReplicationCoordinator::get(startupOpCtx.get())->startup(startupOpCtx.get());
        const unsigned long long missingRepl =
            checkIfReplMissingFromCommandLine(startupOpCtx.get());
        if (missingRepl) {
            log() << startupWarningsLog;
            log() << "** WARNING: mongod started without --replSet yet " << missingRepl
                  << " documents are present in local.system.replset" << startupWarningsLog;
            log() << "**          Restart with --replSet unless you are doing maintenance and "
                  << " no other clients are connected." << startupWarningsLog;
            log() << "**          The TTL collection monitor will not start because of this."
                  << startupWarningsLog;
            log() << "**         ";
            log() << " For more info see http://dochub.mongodb.org/core/ttlcollections";
            log() << startupWarningsLog;
        } else {
            startTTLBackgroundJob();
        }

        if (replSettings.usingReplSets() || (!replSettings.isMaster() && replSettings.isSlave()) ||
            !internalValidateFeaturesAsMaster) {
            serverGlobalParams.validateFeaturesAsMaster.store(false);
        }
    }

    startClientCursorMonitor();

    PeriodicTask::startRunningPeriodicTasks();

    // Set up the periodic runner for background job execution
    auto runner = makePeriodicRunner();
    runner->startup().transitional_ignore();
    serviceContext->setPeriodicRunner(std::move(runner));

    SessionKiller::set(serviceContext,
                       std::make_shared<SessionKiller>(serviceContext, killSessionsLocal));

    GenericCursorManager::set(serviceContext, stdx::make_unique<GenericCursorManagerMongod>());

    // Set up the logical session cache
    LogicalSessionCacheServer kind = LogicalSessionCacheServer::kStandalone;
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        kind = LogicalSessionCacheServer::kSharded;
    } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        kind = LogicalSessionCacheServer::kConfigServer;
    } else if (replSettings.usingReplSets()) {
        kind = LogicalSessionCacheServer::kReplicaSet;
    }

    auto sessionCache = makeLogicalSessionCacheD(serviceContext, kind);
    LogicalSessionCache::set(serviceContext, std::move(sessionCache));

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    auto start = serviceContext->getServiceExecutor()->start();
    if (!start.isOK()) {
        error() << "Failed to start the service executor: " << start;
        return EXIT_NET_ERROR;
    }

    start = serviceContext->getTransportLayer()->start();
    if (!start.isOK()) {
        error() << "Failed to start the listener: " << start.toString();
        return EXIT_NET_ERROR;
    }

    serviceContext->notifyStartupComplete();

#ifndef _WIN32
    mongo::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        log() << "Service running";
    }
#endif

    if (MONGO_FAIL_POINT(shutdownAtStartup)) {
        log() << "starting clean exit via failpoint";
        exitCleanly(EXIT_CLEAN);
    }

    MONGO_IDLE_THREAD_BLOCK;
    return waitForShutdown();
}

ExitCode initAndListen(int listenPort) {
    try {
        return _initAndListen(listenPort);
    } catch (DBException& e) {
        log() << "exception in initAndListen: " << e.toString() << ", terminating";
        return EXIT_UNCAUGHT;
    } catch (std::exception& e) {
        log() << "exception in initAndListen std::exception: " << e.what() << ", terminating";
        return EXIT_UNCAUGHT;
    } catch (int& n) {
        log() << "exception in initAndListen int: " << n << ", terminating";
        return EXIT_UNCAUGHT;
    } catch (...) {
        log() << "exception in initAndListen, terminating";
        return EXIT_UNCAUGHT;
    }
}

#if defined(_WIN32)
ExitCode initService() {
    return initAndListen(serverGlobalParams.port);
}
#endif

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    mongo::forkServerOrDie();
    return Status::OK();
}

/*
 * This function should contain the startup "actions" that we take based on the startup config.
 * It is intended to separate the actions from "storage" and "validation" of our startup
 * configuration.
 */
void startupConfigActions(const std::vector<std::string>& args) {
    // The "command" option is deprecated.  For backward compatibility, still support the "run"
    // and "dbppath" command.  The "run" command is the same as just running mongod, so just
    // falls through.
    if (moe::startupOptionsParsed.count("command")) {
        const auto command = moe::startupOptionsParsed["command"].as<std::vector<std::string>>();

        if (command[0].compare("dbpath") == 0) {
            std::cout << storageGlobalParams.dbpath << endl;
            quickExit(EXIT_SUCCESS);
        }

        if (command[0].compare("run") != 0) {
            std::cout << "Invalid command: " << command[0] << endl;
            printMongodHelp(moe::startupOptions);
            quickExit(EXIT_FAILURE);
        }

        if (command.size() > 1) {
            std::cout << "Too many parameters to 'run' command" << endl;
            printMongodHelp(moe::startupOptions);
            quickExit(EXIT_FAILURE);
        }
    }

#ifdef _WIN32
    ntservice::configureService(initService,
                                moe::startupOptionsParsed,
                                defaultServiceStrings,
                                std::vector<std::string>(),
                                args);
#endif  // _WIN32

#ifdef __linux__
    if (moe::startupOptionsParsed.count("shutdown") &&
        moe::startupOptionsParsed["shutdown"].as<bool>() == true) {
        bool failed = false;

        std::string name =
            (boost::filesystem::path(storageGlobalParams.dbpath) / "mongod.lock").string();
        if (!boost::filesystem::exists(name) || boost::filesystem::file_size(name) == 0)
            failed = true;

        pid_t pid;
        std::string procPath;
        if (!failed) {
            try {
                std::ifstream f(name.c_str());
                f >> pid;
                procPath = (str::stream() << "/proc/" << pid);
                if (!boost::filesystem::exists(procPath))
                    failed = true;
            } catch (const std::exception& e) {
                std::cerr << "Error reading pid from lock file [" << name << "]: " << e.what()
                          << endl;
                failed = true;
            }
        }

        if (failed) {
            std::cerr << "There doesn't seem to be a server running with dbpath: "
                      << storageGlobalParams.dbpath << std::endl;
            quickExit(EXIT_FAILURE);
        }

        std::cout << "killing process with pid: " << pid << endl;
        int ret = kill(pid, SIGTERM);
        if (ret) {
            int e = errno;
            std::cerr << "failed to kill process: " << errnoWithDescription(e) << endl;
            quickExit(EXIT_FAILURE);
        }

        while (boost::filesystem::exists(procPath)) {
            sleepsecs(1);
        }

        quickExit(EXIT_SUCCESS);
    }
#endif
}

auto makeReplicationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "replexec";
    tpOptions.maxThreads = 50;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(serviceContext));
    return stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        stdx::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface(
            "NetworkInterfaceASIO-Replication", nullptr, std::move(hookList)));
}

MONGO_INITIALIZER_WITH_PREREQUISITES(CreateReplicationManager,
                                     ("SetGlobalEnvironment", "SSLManager", "default"))
(InitializerContext* context) {
    auto serviceContext = getGlobalServiceContext();
    repl::StorageInterface::set(serviceContext, stdx::make_unique<repl::StorageInterfaceImpl>());
    auto storageInterface = repl::StorageInterface::get(serviceContext);

    auto consistencyMarkers =
        stdx::make_unique<repl::ReplicationConsistencyMarkersImpl>(storageInterface);
    auto recovery = stdx::make_unique<repl::ReplicationRecoveryImpl>(storageInterface,
                                                                     consistencyMarkers.get());
    repl::ReplicationProcess::set(
        serviceContext,
        stdx::make_unique<repl::ReplicationProcess>(
            storageInterface, std::move(consistencyMarkers), std::move(recovery)));
    auto replicationProcess = repl::ReplicationProcess::get(serviceContext);

    repl::DropPendingCollectionReaper::set(
        serviceContext, stdx::make_unique<repl::DropPendingCollectionReaper>(storageInterface));
    auto dropPendingCollectionReaper = repl::DropPendingCollectionReaper::get(serviceContext);

    repl::TopologyCoordinator::Options topoCoordOptions;
    topoCoordOptions.maxSyncSourceLagSecs = Seconds(repl::maxSyncSourceLagSecs);
    topoCoordOptions.clusterRole = serverGlobalParams.clusterRole;

    auto logicalClock = stdx::make_unique<LogicalClock>(serviceContext);
    LogicalClock::set(serviceContext, std::move(logicalClock));

    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorImpl>(
        serviceContext,
        getGlobalReplSettings(),
        stdx::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(
            serviceContext, dropPendingCollectionReaper, storageInterface, replicationProcess),
        makeReplicationExecutor(serviceContext),
        stdx::make_unique<repl::TopologyCoordinator>(topoCoordOptions),
        replicationProcess,
        storageInterface,
        static_cast<int64_t>(curTimeMillis64()));
    repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));
    repl::setOplogCollectionName();
    return Status::OK();
}

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, MONGO_NO_PREREQUISITES, ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
    return Status::OK();
}
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

// NOTE: This function may be called at any time after registerShutdownTask is called below. It
// must not depend on the prior execution of mongo initializers or the existence of threads.
void shutdownTask() {
    Client::initThreadIfNotAlready();

    auto const client = Client::getCurrent();
    auto const serviceContext = client->getServiceContext();

    // Shutdown the TransportLayer so that new connections aren't accepted
    if (auto tl = serviceContext->getTransportLayer()) {
        log(LogComponent::kNetwork) << "shutdown: going to close listening sockets...";
        tl->shutdown();
    }

    // Shut down the global dbclient pool so callers stop waiting for connections.
    globalConnPool.shutdown();

    if (serviceContext->getGlobalStorageEngine()) {
        ServiceContext::UniqueOperationContext uniqueOpCtx;
        OperationContext* opCtx = client->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = client->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }

        if (serverGlobalParams.featureCompatibility.getVersion() !=
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36) {
            log(LogComponent::kReplication) << "shutdown: removing all drop-pending collections...";
            repl::DropPendingCollectionReaper::get(serviceContext)
                ->dropCollectionsOlderThan(opCtx, repl::OpTime::max());

            // If we are in fCV 3.4, drop the 'checkpointTimestamp' collection so if we downgrade
            // and then upgrade again, we do not trust a stale 'checkpointTimestamp'.
            log(LogComponent::kReplication)
                << "shutdown: removing checkpointTimestamp collection...";
            Status status =
                repl::StorageInterface::get(serviceContext)
                    ->dropCollection(opCtx,
                                     NamespaceString(repl::ReplicationConsistencyMarkersImpl::
                                                         kDefaultCheckpointTimestampNamespace));
            if (!status.isOK()) {
                warning(LogComponent::kReplication)
                    << "shutdown: dropping checkpointTimestamp collection failed: "
                    << redact(status.toString());
            }
        }

        // This can wait a long time while we drain the secondary's apply queue, especially if it
        // is building an index.
        repl::ReplicationCoordinator::get(serviceContext)->shutdown(opCtx);

        ShardingState::get(serviceContext)->shutDown(opCtx);
    }

    serviceContext->setKillAllOperations();

    // Shut down the background periodic task runner
    if (auto runner = serviceContext->getPeriodicRunner()) {
        runner->shutdown();
    }

    ReplicaSetMonitor::shutdown();

    if (auto sr = Grid::get(serviceContext)->shardRegistry()) {
        sr->shutdown();
    }

    // Validator shutdown must be called after setKillAllOperations is called. Otherwise, this can
    // deadlock.
    if (auto validator = LogicalTimeValidator::get(serviceContext)) {
        validator->shutDown();
    }

#if __has_feature(address_sanitizer)
    // When running under address sanitizer, we get false positive leaks due to disorder around
    // the lifecycle of a connection and request. When we are running under ASAN, we try a lot
    // harder to dry up the server from active connections before going on to really shut down.

    // Shutdown the Service Entry Point and its sessions and give it a grace period to complete.
    if (auto sep = serviceContext->getServiceEntryPoint()) {
        if (!sep->shutdown(Seconds(10))) {
            log(LogComponent::kNetwork)
                << "Service entry point failed to shutdown within timelimit.";
        }
    }

    // Shutdown and wait for the service executor to exit
    if (auto svcExec = serviceContext->getServiceExecutor()) {
        Status status = svcExec->shutdown(Seconds(5));
        if (!status.isOK()) {
            log(LogComponent::kNetwork) << "Service executor failed to shutdown within timelimit: "
                                        << status.reason();
        }
    }
#endif

    // Shutdown Full-Time Data Capture
    stopMongoDFTDC();

    HealthLog::get(serviceContext).shutdown();

    // We should always be able to acquire the global lock at shutdown.
    //
    // TODO: This call chain uses the locker directly, because we do not want to start an
    // operation context, which also instantiates a recovery unit. Also, using the
    // lockGlobalBegin/lockGlobalComplete sequence, we avoid taking the flush lock.
    //
    // For a Windows service, dbexit does not call exit(), so we must leak the lock outside
    // of this function to prevent any operations from running that need a lock.
    //
    DefaultLockerImpl* globalLocker = new DefaultLockerImpl();
    LockResult result = globalLocker->lockGlobalBegin(MODE_X, Milliseconds::max());
    if (result == LOCK_WAITING) {
        result = globalLocker->lockGlobalComplete(Milliseconds::max());
    }

    invariant(LOCK_OK == result);

    // Global storage engine may not be started in all cases before we exit
    if (serviceContext->getGlobalStorageEngine()) {
        serviceContext->shutdownGlobalStorageEngineCleanly();
    }

    // We drop the scope cache because leak sanitizer can't see across the
    // thread we use for proxying MozJS requests. Dropping the cache cleans up
    // the memory and makes leak sanitizer happy.
    ScriptEngine::dropScopeCache();

    log(LogComponent::kControl) << "now exiting";

    audit::logShutdown(client);
}


}  // namespace

int mongoDbMain(int argc, char* argv[], char** envp) {
    registerShutdownTask(shutdownTask);

    setupSignalHandlers();

    srand(static_cast<unsigned>(curTimeMicros64()));

    Status status = mongo::runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe(LogComponent::kControl) << "Failed global initialization: " << status;
        quickExit(EXIT_FAILURE);
    }

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    if (!initializeServerGlobalState())
        quickExit(EXIT_FAILURE);

    // Per SERVER-7434, startSignalProcessingThread() must run after any forks
    // (initializeServerGlobalState()) and before creation of any other threads.
    startSignalProcessingThread();

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // exits directly and so never reaches here either.
    }
#endif

    StartupTest::runTests();
    ExitCode exitCode = initAndListen(serverGlobalParams.port);
    exitCleanly(exitCode);
    return 0;
}

}  // namespace mongo
