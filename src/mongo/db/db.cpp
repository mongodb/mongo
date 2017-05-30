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
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/diag_log.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/initialize_snmp.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/keys_collection_manager.h"
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
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
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

using std::unique_ptr;
using std::cout;
using std::cerr;
using std::endl;
using std::list;
using std::string;
using std::stringstream;
using std::vector;

using logger::LogComponent;

extern int diagLogging;

namespace {

const NamespaceString startupLogCollectionName("local.startup_log");
const NamespaceString kSystemReplSetCollection("local.system.replset");

#ifdef _WIN32
ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoDB", L"MongoDB", L"MongoDB Server"};
#endif

void logStartup(OperationContext* opCtx) {
    BSONObjBuilder toLog;
    stringstream id;
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

    list<string> collections;
    db->getDatabaseCatalogEntry()->getCollectionNamespaces(&collections);

    for (list<string>::iterator i = collections.begin(); i != collections.end(); ++i) {
        const string& collectionName = *i;
        NamespaceString ns(collectionName);

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

        log() << "WARNING: the collection '" << *i << "' lacks a unique index on _id."
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

void repairDatabasesAndCheckVersion(OperationContext* opCtx) {
    LOG(1) << "enter repairDatabases (to check pdfile version #)";

    Lock::GlobalWrite lk(opCtx);

    vector<string> dbNames;

    StorageEngine* storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&dbNames);

    // Repair all databases first, so that we do not try to open them if they are in bad shape
    if (storageGlobalParams.repair) {
        invariant(!storageGlobalParams.readOnly);
        for (vector<string>::const_iterator i = dbNames.begin(); i != dbNames.end(); ++i) {
            const string dbName = *i;
            LOG(1) << "    Repairing database: " << dbName;

            fassert(18506, repairDatabase(opCtx, storageEngine, dbName));
        }
    }

    const repl::ReplSettings& replSettings = repl::getGlobalReplicationCoordinator()->getSettings();

    if (!storageGlobalParams.readOnly) {
        // We open the "local" database before calling checkIfReplMissingFromCommandLine() to ensure
        // the in-memory catalog entries for the 'kSystemReplSetCollection' collection have been
        // populated if the collection exists. If the "local" database didn't exist at this point
        // yet, then it will be created. If the mongod is running in a read-only mode, then it is
        // fine to not open the "local" database and populate the catalog entries because we won't
        // attempt to drop the temporary collections anyway.
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

    for (vector<string>::const_iterator i = dbNames.begin(); i != dbNames.end(); ++i) {
        const string dbName = *i;
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
            return;
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
                    auto version = FeatureCompatibilityVersion::parse(featureCompatibilityVersion);
                    if (!version.isOK()) {
                        severe() << version.getStatus();
                        fassertFailedNoTrace(40283);
                    }
                    serverGlobalParams.featureCompatibility.version.store(version.getValue());

                    // Update schemaVersion parameter.
                    serverGlobalParams.featureCompatibility.isSchemaVersion36.store(
                        serverGlobalParams.featureCompatibility.version.load() ==
                        ServerGlobalParams::FeatureCompatibility::Version::k36);
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
            const string plugin = IndexNames::findPluginName(key);

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

    LOG(1) << "done repairDatabases";
}

void _initWireSpec() {
    WireSpec& spec = WireSpec::instance();

    spec.isInternalClient = true;
}

MONGO_FP_DECLARE(shutdownAtStartup);

ExitCode _initAndListen(int listenPort) {
    Client::initThread("initandlisten");

    _initWireSpec();
    auto globalServiceContext = checked_cast<ServiceContextMongoD*>(getGlobalServiceContext());

    globalServiceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds(10)));
    globalServiceContext->setOpObserver(stdx::make_unique<OpObserverImpl>());

    DBDirectClientFactory::get(globalServiceContext)
        .registerImplementation([](OperationContext* opCtx) {
            return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
        });

    const repl::ReplSettings& replSettings = repl::getGlobalReplicationCoordinator()->getSettings();

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

    globalServiceContext->createLockFile();

    globalServiceContext->setServiceEntryPoint(
        stdx::make_unique<ServiceEntryPointMongod>(globalServiceContext));

    auto tl = transport::TransportLayerManager::createWithConfig(&serverGlobalParams,
                                                                 globalServiceContext);
    auto res = tl->setup();
    if (!res.isOK()) {
        error() << "Failed to set up listener: " << res;
        return EXIT_NET_ERROR;
    }
    globalServiceContext->setTransportLayer(std::move(tl));

    globalServiceContext->initializeGlobalStorageEngine();

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
    if (EncryptionHooks::get(getGlobalServiceContext())->restartRequired()) {
        exitCleanly(EXIT_CLEAN);
    }
#endif

    // Warn if we detect configurations for multiple registered storage engines in
    // the same configuration file/environment.
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
            if (globalServiceContext->isRegisteredStorageEngine(e.fieldName())) {
                warning() << "Detected configuration for non-active storage engine "
                          << e.fieldName() << " when current storage engine is "
                          << storageGlobalParams.engine;
            }
        }
    }

    if (!globalServiceContext->getGlobalStorageEngine()->getSnapshotManager()) {
        if (moe::startupOptionsParsed.count("replication.enableMajorityReadConcern") &&
            moe::startupOptionsParsed["replication.enableMajorityReadConcern"].as<bool>()) {
            // Note: we are intentionally only erroring if the user explicitly requested that we
            // enable majority read concern. We do not error if the they are implicitly enabled for
            // CSRS because a required step in the upgrade procedure can involve an mmapv1 node in
            // the CSRS in the REMOVED state. This is handled by the TopologyCoordinator.
            invariant(replSettings.isMajorityReadConcernEnabled());
            severe() << "Majority read concern requires a storage engine that supports"
                     << " snapshots, such as wiredTiger. " << storageGlobalParams.engine
                     << " does not support snapshots.";
            exitCleanly(EXIT_BADOPTIONS);
        }
    }

    logMongodStartupWarnings(storageGlobalParams, serverGlobalParams);

    {
        stringstream ss;
        ss << endl;
        ss << "*********************************************************************" << endl;
        ss << " ERROR: dbpath (" << storageGlobalParams.dbpath << ") does not exist." << endl;
        ss << " Create this directory or give existing directory in --dbpath." << endl;
        ss << " See http://dochub.mongodb.org/core/startingandstoppingmongo" << endl;
        ss << "*********************************************************************" << endl;
        uassert(10296, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.dbpath));
    }

    {
        stringstream ss;
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

    auto startupOpCtx = globalServiceContext->makeOperationContext(&cc());

    repairDatabasesAndCheckVersion(startupOpCtx.get());

    if (storageGlobalParams.upgrade) {
        log() << "finished checking dbs";
        exitCleanly(EXIT_CLEAN);
    }

    uassertStatusOK(getGlobalAuthorizationManager()->initialize(startupOpCtx.get()));

    /* this is for security on certain platforms (nonce generation) */
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));

    AuthorizationManager* globalAuthzManager = getGlobalAuthorizationManager();
    if (globalAuthzManager->shouldValidateAuthSchemaOnStartup()) {
        Status status = verifySystemIndexes(startupOpCtx.get());
        if (!status.isOK()) {
            log() << redact(status);
            if (status.code() == ErrorCodes::AuthSchemaIncompatible) {
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

    SessionCatalog::create(globalServiceContext);

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
        }

        repl::getGlobalReplicationCoordinator()->startup(startupOpCtx.get());

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

        if (!replSettings.usingReplSets() && !replSettings.isSlave() &&
            storageGlobalParams.engine != "devnull") {
            Lock::GlobalWrite lk(startupOpCtx.get());
            FeatureCompatibilityVersion::setIfCleanStartup(
                startupOpCtx.get(), repl::StorageInterface::get(globalServiceContext));
        }

        if (replSettings.usingReplSets() || (!replSettings.isMaster() && replSettings.isSlave()) ||
            !internalValidateFeaturesAsMaster) {
            serverGlobalParams.featureCompatibility.validateFeaturesAsMaster.store(false);
        }
    }

    startClientCursorMonitor();

    PeriodicTask::startRunningPeriodicTasks();

    // Set up the periodic runner for background job execution
    auto runner = makePeriodicRunner();
    runner->startup().transitional_ignore();
    globalServiceContext->setPeriodicRunner(std::move(runner));

    // Set up the logical session cache
    LogicalSessionCacheServer kind = LogicalSessionCacheServer::kStandalone;
    if (shardingInitialized) {
        kind = LogicalSessionCacheServer::kSharded;
    } else if (replSettings.usingReplSets()) {
        kind = LogicalSessionCacheServer::kReplicaSet;
    }

    auto sessionCache = makeLogicalSessionCacheD(kind);
    LogicalSessionCache::set(globalServiceContext, std::move(sessionCache));

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    auto start = globalServiceContext->getTransportLayer()->start();
    if (!start.isOK()) {
        error() << "Failed to start the listener: " << start.toString();
        return EXIT_NET_ERROR;
    }

    if (globalServiceContext->getServiceExecutor()) {
        start = globalServiceContext->getServiceExecutor()->start();
        if (!start.isOK()) {
            error() << "Failed to start the service executor: " << start;
            return EXIT_NET_ERROR;
        }
    }

    globalServiceContext->notifyStartupComplete();
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
 * This function should contain the startup "actions" that we take based on the startup config.  It
 * is intended to separate the actions from "storage" and "validation" of our startup configuration.
 */
static void startupConfigActions(const std::vector<std::string>& args) {
    // The "command" option is deprecated.  For backward compatibility, still support the "run"
    // and "dbppath" command.  The "run" command is the same as just running mongod, so just
    // falls through.
    if (moe::startupOptionsParsed.count("command")) {
        vector<string> command = moe::startupOptionsParsed["command"].as<vector<string>>();

        if (command[0].compare("dbpath") == 0) {
            cout << storageGlobalParams.dbpath << endl;
            quickExit(EXIT_SUCCESS);
        }

        if (command[0].compare("run") != 0) {
            cout << "Invalid command: " << command[0] << endl;
            printMongodHelp(moe::startupOptions);
            quickExit(EXIT_FAILURE);
        }

        if (command.size() > 1) {
            cout << "Too many parameters to 'run' command" << endl;
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

        string name =
            (boost::filesystem::path(storageGlobalParams.dbpath) / "mongod.lock").string();
        if (!boost::filesystem::exists(name) || boost::filesystem::file_size(name) == 0)
            failed = true;

        pid_t pid;
        string procPath;
        if (!failed) {
            try {
                std::ifstream f(name.c_str());
                f >> pid;
                procPath = (str::stream() << "/proc/" << pid);
                if (!boost::filesystem::exists(procPath))
                    failed = true;
            } catch (const std::exception& e) {
                cerr << "Error reading pid from lock file [" << name << "]: " << e.what() << endl;
                failed = true;
            }
        }

        if (failed) {
            std::cerr << "There doesn't seem to be a server running with dbpath: "
                      << storageGlobalParams.dbpath << std::endl;
            quickExit(EXIT_FAILURE);
        }

        cout << "killing process with pid: " << pid << endl;
        int ret = kill(pid, SIGTERM);
        if (ret) {
            int e = errno;
            cerr << "failed to kill process: " << errnoWithDescription(e) << endl;
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

    repl::ReplicationProcess::set(
        serviceContext,
        stdx::make_unique<repl::ReplicationProcess>(
            storageInterface,
            stdx::make_unique<repl::ReplicationConsistencyMarkersImpl>(storageInterface)));
    auto replicationProcess = repl::ReplicationProcess::get(serviceContext);

    repl::DropPendingCollectionReaper::set(
        serviceContext, stdx::make_unique<repl::DropPendingCollectionReaper>(storageInterface));
    auto dropPendingCollectionReaper = repl::DropPendingCollectionReaper::get(serviceContext);

    repl::TopologyCoordinatorImpl::Options topoCoordOptions;
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
        stdx::make_unique<repl::TopologyCoordinatorImpl>(topoCoordOptions),
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

// NOTE: This function may be called at any time after
// registerShutdownTask is called below. It must not depend on the
// prior execution of mongo initializers or the existence of threads.
static void shutdownTask() {
    auto serviceContext = getGlobalServiceContext();

    Client::initThreadIfNotAlready();
    Client& client = cc();

    ServiceContext::UniqueOperationContext uniqueTxn;
    OperationContext* opCtx = client.getOperationContext();
    if (!opCtx && serviceContext->getGlobalStorageEngine()) {
        uniqueTxn = client.makeOperationContext();
        opCtx = uniqueTxn.get();
    }

    log(LogComponent::kNetwork) << "shutdown: going to close listening sockets..." << endl;
    ListeningSockets::get()->closeAll();

    log(LogComponent::kNetwork) << "shutdown: going to flush diaglog..." << endl;
    _diaglog.flush();

    if (opCtx) {
        if (serverGlobalParams.featureCompatibility.version.load() ==
            ServerGlobalParams::FeatureCompatibility::Version::k34) {
            log(LogComponent::kReplication) << "shutdown: removing all drop-pending collections...";
            repl::DropPendingCollectionReaper::get(opCtx)->dropCollectionsOlderThan(
                opCtx, repl::OpTime::max());
        }

        // This can wait a long time while we drain the secondary's apply queue, especially if it is
        // building an index.
        repl::ReplicationCoordinator::get(opCtx)->shutdown(opCtx);
    }

    if (serviceContext) {
        serviceContext->setKillAllOperations();

        // Shut down the background periodic task runner.
        auto runner = serviceContext->getPeriodicRunner();
        if (runner) {
            runner->shutdown();
        }
    }

    ReplicaSetMonitor::shutdown();
    if (auto sr = grid.shardRegistry()) {  // TODO: race: sr is a naked pointer
        sr->shutdown();
    }

    // Validator shutdown must be called after setKillAllOperations is called. Otherwise, this can
    // deadlock.
    if (auto validator = LogicalTimeValidator::get(serviceContext)) {
        validator->shutDown();
    }

#if __has_feature(address_sanitizer)
    auto sep = checked_cast<ServiceEntryPointImpl*>(serviceContext->getServiceEntryPoint());
    auto tl = serviceContext->getTransportLayer();
    if (sep && tl) {
        // When running under address sanitizer, we get false positive leaks due to disorder around
        // the lifecycle of a connection and request. When we are running under ASAN, we try a lot
        // harder to dry up the server from active connections before going on to really shut down.

        log(LogComponent::kNetwork)
            << "shutdown: going to close all sockets because ASAN is active...";

        // Shutdown the TransportLayer so that new connections aren't accepted
        tl->shutdown();

        // Request that all sessions end.
        sep->endAllSessions(transport::Session::kEmptyTagMask);

        // Close all sockets in a detached thread, and then wait for the number of active
        // connections to reach zero. Give the detached background thread a 10 second deadline. If
        // we haven't closed drained all active operations within that deadline, just keep going
        // with shutdown: the OS will do it for us when the process terminates.
        stdx::packaged_task<void()> dryOutTask([sep] {
            // There isn't currently a way to wait on the TicketHolder to have all its tickets back,
            // unfortunately. So, busy wait in this detached thread.
            while (true) {
                const auto runningWorkers = sep->getNumberOfConnections();

                if (runningWorkers == 0) {
                    log(LogComponent::kNetwork) << "shutdown: no running workers found...";
                    break;
                }
                log(LogComponent::kNetwork) << "shutdown: still waiting on " << runningWorkers
                                            << " active workers to drain... ";
                mongo::sleepFor(Milliseconds(250));
            }
        });

        auto dryNotification = dryOutTask.get_future();
        stdx::thread(std::move(dryOutTask)).detach();
        if (dryNotification.wait_for(Seconds(10).toSystemDuration()) !=
            stdx::future_status::ready) {
            log(LogComponent::kNetwork) << "shutdown: exhausted grace period for"
                                        << " active workers to drain; continuing with shutdown... ";
        }

        // Shutdown and wait for the service executor to exit
        auto svcExec = serviceContext->getServiceExecutor();
        if (svcExec) {
            fassertStatusOK(40550, svcExec->shutdown());
        }
    }
#endif

    // Shutdown Full-Time Data Capture
    stopMongoDFTDC();

    if (opCtx) {
        ShardingState::get(opCtx)->shutDown(opCtx);
    }

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

    if (serviceContext && serviceContext->getGlobalStorageEngine()) {
        serviceContext->shutdownGlobalStorageEngineCleanly();
    }

    // We drop the scope cache because leak sanitizer can't see across the
    // thread we use for proxying MozJS requests. Dropping the cache cleans up
    // the memory and makes leak sanitizer happy.
    ScriptEngine::dropScopeCache();

    log(LogComponent::kControl) << "now exiting" << endl;

    audit::logShutdown(&cc());
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
