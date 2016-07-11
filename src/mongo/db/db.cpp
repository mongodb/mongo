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
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/restapi.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/snapshots.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/ttl.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer_legacy.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostname_canonicalization_worker.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/static_observer.h"
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

void (*snmpInit)() = NULL;

extern int diagLogging;

static const NamespaceString startupLogCollectionName("local.startup_log");

#ifdef _WIN32
ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoDB", L"MongoDB", L"MongoDB Server"};
#endif

Timer startupSrandTimer;

static void logStartup(OperationContext* txn) {
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
    appendBuildInfo(buildinfo);
    appendStorageEngineList(&buildinfo);
    buildinfo.doneFast();

    BSONObj o = toLog.obj();

    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lk(txn->lockState());
    AutoGetOrCreateDb autoDb(txn, startupLogCollectionName.db(), mongo::MODE_X);
    Database* db = autoDb.getDb();
    Collection* collection = db->getCollection(startupLogCollectionName);
    WriteUnitOfWork wunit(txn);
    if (!collection) {
        BSONObj options = BSON("capped" << true << "size" << 10 * 1024 * 1024);
        bool shouldReplicateWrites = txn->writesAreReplicated();
        txn->setReplicatedWrites(false);
        ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, txn, shouldReplicateWrites);
        uassertStatusOK(userCreateNS(txn, db, startupLogCollectionName.ns(), options));
        collection = db->getCollection(startupLogCollectionName);
    }
    invariant(collection);

    OpDebug* const nullOpDebug = nullptr;
    uassertStatusOK(collection->insertDocument(txn, o, nullOpDebug, false));
    wunit.commit();
}

static void checkForIdIndexes(OperationContext* txn, Database* db) {
    if (db->name() == "local") {
        // we do not need an _id index on anything in the local database
        return;
    }

    list<string> collections;
    db->getDatabaseCatalogEntry()->getCollectionNamespaces(&collections);

    // for each collection, ensure there is a $_id_ index
    for (list<string>::iterator i = collections.begin(); i != collections.end(); ++i) {
        const string& collectionName = *i;
        NamespaceString ns(collectionName);
        if (ns.isSystem())
            continue;

        Collection* coll = db->getCollection(collectionName);
        if (!coll)
            continue;

        if (coll->getIndexCatalog()->findIdIndex(txn))
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
static unsigned long long checkIfReplMissingFromCommandLine(OperationContext* txn) {
    // This is helpful for the query below to work as you can't open files when readlocked
    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lk(txn->lockState());
    if (!repl::getGlobalReplicationCoordinator()->getSettings().usingReplSets()) {
        DBDirectClient c(txn);
        return c.count("local.system.replset");
    }
    return 0;
}

/**
 * Due to SERVER-23274, versions 3.2.0 through 3.2.4 of MongoDB incorrectly mark the final output
 * collections of aggregations with $out stages as temporary on most replica set secondaries. Rather
 * than risk deleting collections that the user did not intend to be temporary when newer nodes
 * start up or get promoted to be replica set primaries, newer nodes clear the temp flags left by
 * these versions.
 */
static bool isSubjectToSERVER23299(OperationContext* txn) {
    if (storageGlobalParams.readOnly) {
        return false;
    }
    dbHolder().openDb(txn, startupLogCollectionName.db());
    AutoGetCollectionForRead autoColl(txn, startupLogCollectionName);
    // No startup log or an empty one means either that the user was not running an affected
    // version, or that they manually deleted the startup collection since they last started an
    // affected version.
    LOG(1) << "Checking node for SERVER-23299 eligibility";
    if (!autoColl.getCollection()) {
        LOG(1) << "Didn't find " << startupLogCollectionName;
        return false;
    }
    LOG(1) << "Checking node for SERVER-23299 applicability - reading startup log";
    BSONObj lastStartupLogDoc;
    if (!Helpers::getLast(txn, startupLogCollectionName.ns().c_str(), lastStartupLogDoc)) {
        return false;
    }
    std::vector<int> versionComponents;
    try {
        for (auto elem : lastStartupLogDoc["buildinfo"]["versionArray"].Obj()) {
            versionComponents.push_back(elem.Int());
        }
        uassert(40050,
                str::stream() << "Expected three elements in buildinfo.versionArray; found "
                              << versionComponents.size(),
                versionComponents.size() >= 3);
    } catch (const DBException& ex) {
        log() << "Last entry of " << startupLogCollectionName
              << " has no well-formed  buildinfo.versionArray field; ignoring " << causedBy(ex);
        return false;
    }
    LOG(1)
        << "Checking node for SERVER-23299 applicability - checking version 3.2.x for x in [0, 4]";
    if (versionComponents[0] != 3)
        return false;
    if (versionComponents[1] != 2)
        return false;
    if (versionComponents[2] > 4)
        return false;
    LOG(1) << "Node eligible for SERVER-23299";
    return true;
}

static void handleSERVER23299ForDb(OperationContext* txn, Database* db) {
    log() << "Scanning " << db->name() << " db for SERVER-23299 eligibility";
    const auto dbEntry = db->getDatabaseCatalogEntry();
    list<string> collNames;
    dbEntry->getCollectionNamespaces(&collNames);
    for (const auto& collName : collNames) {
        const auto collEntry = dbEntry->getCollectionCatalogEntry(collName);
        const auto collOptions = collEntry->getCollectionOptions(txn);
        if (!collOptions.temp)
            continue;
        log() << "Marking collection " << collName << " as permanent per SERVER-23299";
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wuow(txn);
            collEntry->clearTempFlag(txn);
            wuow.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "repair SERVER-23299", collEntry->ns().ns());
    }
    log() << "Done scanning " << db->name() << " for SERVER-23299 eligibility";
}

static void repairDatabasesAndCheckVersion(OperationContext* txn) {
    LOG(1) << "enter repairDatabases (to check pdfile version #)" << endl;

    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lk(txn->lockState());

    vector<string> dbNames;

    StorageEngine* storageEngine = txn->getServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&dbNames);

    // Repair all databases first, so that we do not try to open them if they are in bad shape
    if (storageGlobalParams.repair) {
        invariant(!storageGlobalParams.readOnly);
        for (vector<string>::const_iterator i = dbNames.begin(); i != dbNames.end(); ++i) {
            const string dbName = *i;
            LOG(1) << "    Repairing database: " << dbName << endl;

            fassert(18506, repairDatabase(txn, storageEngine, dbName));
        }
    }

    const repl::ReplSettings& replSettings = repl::getGlobalReplicationCoordinator()->getSettings();

    // On replica set members we only clear temp collections on DBs other than "local" during
    // promotion to primary. On pure slaves, they are only cleared when the oplog tells them
    // to. The local DB is special because it is not replicated.  See SERVER-10927 for more
    // details.
    const bool shouldClearNonLocalTmpCollections =
        !(checkIfReplMissingFromCommandLine(txn) || replSettings.usingReplSets() ||
          replSettings.isSlave());

    const bool shouldDoCleanupForSERVER23299 = isSubjectToSERVER23299(txn);

    for (vector<string>::const_iterator i = dbNames.begin(); i != dbNames.end(); ++i) {
        const string dbName = *i;
        LOG(1) << "    Recovering database: " << dbName << endl;

        Database* db = dbHolder().openDb(txn, dbName);
        invariant(db);

        // First thing after opening the database is to check for file compatibility,
        // otherwise we might crash if this is a deprecated format.
        auto status = db->getDatabaseCatalogEntry()->currentFilesCompatible(txn);
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
                     << status;
            severe() << "Please consult our documentation when trying to downgrade to a previous"
                        " major release";
            quickExit(EXIT_NEED_UPGRADE);
            return;
        }

        // Major versions match, check indexes
        const string systemIndexes = db->name() + ".system.indexes";

        Collection* coll = db->getCollection(systemIndexes);
        unique_ptr<PlanExecutor> exec(
            InternalPlanner::collectionScan(txn, systemIndexes, coll, PlanExecutor::YIELD_MANUAL));

        BSONObj index;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&index, NULL))) {
            const BSONObj key = index.getObjectField("key");
            const string plugin = IndexNames::findPluginName(key);

            if (db->getDatabaseCatalogEntry()->isOlderThan24(txn)) {
                if (IndexNames::existedBefore24(plugin)) {
                    continue;
                }

                log() << "Index " << index << " claims to be of type '" << plugin << "', "
                      << "which is either invalid or did not exist before v2.4. "
                      << "See the upgrade section: "
                      << "http://dochub.mongodb.org/core/upgrade-2.4" << startupWarningsLog;
            }

            const Status keyStatus = validateKeyPattern(key);
            if (!keyStatus.isOK()) {
                log() << "Problem with index " << index << ": " << keyStatus.reason()
                      << " This index can still be used however it cannot be rebuilt."
                      << " For more info see"
                      << " http://dochub.mongodb.org/core/index-validation" << startupWarningsLog;
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
            // We only care about the _id index if we are in a replset
            checkForIdIndexes(txn, db);
            // Ensure oplog is capped (mmap does not guarantee order of inserts on noncapped
            // collections)
            repl::checkForCappedOplog(txn);
        }

        if (shouldDoCleanupForSERVER23299) {
            handleSERVER23299ForDb(txn, db);
        }

        if (!storageGlobalParams.readOnly &&
            (shouldClearNonLocalTmpCollections || dbName == "local")) {
            db->clearTmpCollections(txn);
        }
    }

    LOG(1) << "done repairDatabases" << endl;
}

static void _initWireSpec() {
    WireSpec& spec = WireSpec::instance();
    // accept from any version
    spec.minWireVersionIncoming = RELEASE_2_4_AND_BEFORE;
    spec.maxWireVersionIncoming = COMMANDS_ACCEPT_WRITE_CONCERN;
    // connect to any version
    spec.minWireVersionOutgoing = RELEASE_2_4_AND_BEFORE;
    spec.maxWireVersionOutgoing = COMMANDS_ACCEPT_WRITE_CONCERN;
}


static ExitCode _initAndListen(int listenPort) {
    Client::initThread("initandlisten");

    _initWireSpec();
    auto globalServiceContext = getGlobalServiceContext();

    globalServiceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds(10)));
    globalServiceContext->setOpObserver(stdx::make_unique<OpObserver>());

    DBDirectClientFactory::get(globalServiceContext)
        .registerImplementation([](OperationContext* txn) {
            return std::unique_ptr<DBClientBase>(new DBDirectClient(txn));
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
    printTargetMinOS();
#endif

    logProcessDetails();

    checked_cast<ServiceContextMongoD*>(getGlobalServiceContext())->createLockFile();

    transport::TransportLayerLegacy::Options options;
    options.port = listenPort;
    options.ipList = serverGlobalParams.bind_ip;

    // Create, start, and attach the TL
    auto transportLayer = stdx::make_unique<transport::TransportLayerLegacy>(
        options,
        std::make_shared<ServiceEntryPointMongod>(getGlobalServiceContext()->getTransportLayer()));
    auto res = transportLayer->setup();
    if (!res.isOK()) {
        error() << "Failed to set up listener: " << res.toString();
        return EXIT_NET_ERROR;
    }

    std::shared_ptr<DbWebServer> dbWebServer;
    if (serverGlobalParams.isHttpInterfaceEnabled) {
        dbWebServer.reset(new DbWebServer(serverGlobalParams.bind_ip,
                                          serverGlobalParams.port + 1000,
                                          getGlobalServiceContext(),
                                          new RestAdminAccess()));
        if (!dbWebServer->setupSockets()) {
            error() << "Failed to set up sockets for HTTP interface during startup.";
            return EXIT_NET_ERROR;
        }
    }

    getGlobalServiceContext()->initializeGlobalStorageEngine();

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
    if (WiredTigerCustomizationHooks::get(getGlobalServiceContext())->restartRequired()) {
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
            if (getGlobalServiceContext()->isRegisteredStorageEngine(e.fieldName())) {
                warning() << "Detected configuration for non-active storage engine "
                          << e.fieldName() << " when current storage engine is "
                          << storageGlobalParams.engine;
            }
        }
    }

    if (!getGlobalServiceContext()->getGlobalStorageEngine()->getSnapshotManager()) {
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

    // TODO:  This should go into a MONGO_INITIALIZER once we have figured out the correct
    // dependencies.
    if (snmpInit) {
        snmpInit();
    }

    if (!storageGlobalParams.readOnly) {
        boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");
    }

    if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalRecoverOnly)
        return EXIT_NET_ERROR;

    if (mongodGlobalParams.scriptingEnabled) {
        ScriptEngine::setup();
    }

    auto startupOpCtx = getGlobalServiceContext()->makeOperationContext(&cc());

    repairDatabasesAndCheckVersion(startupOpCtx.get());

    if (storageGlobalParams.upgrade) {
        log() << "finished checking dbs" << endl;
        exitCleanly(EXIT_CLEAN);
    }

    uassertStatusOK(getGlobalAuthorizationManager()->initialize(startupOpCtx.get()));

    /* this is for security on certain platforms (nonce generation) */
    srand((unsigned)(curTimeMicros64() ^ startupSrandTimer.micros()));

    // The snapshot thread provides historical collection level and lock statistics for use
    // by the web interface. Only needed when HTTP is enabled.
    if (serverGlobalParams.isHttpInterfaceEnabled) {
        statsSnapshotThread.go();

        invariant(dbWebServer);
        stdx::thread web(stdx::bind(&webServerListenThread, dbWebServer));
        web.detach();
    }

    {
#ifndef _WIN32
        mongo::signalForkSuccess();
#endif

        Status status = authindex::verifySystemIndexes(startupOpCtx.get());
        if (!status.isOK()) {
            log() << status.reason();
            exitCleanly(EXIT_NEED_UPGRADE);
        }

        // SERVER-14090: Verify that auth schema version is schemaVersion26Final.
        int foundSchemaVersion;
        status = getGlobalAuthorizationManager()->getAuthorizationVersion(startupOpCtx.get(),
                                                                          &foundSchemaVersion);
        if (!status.isOK()) {
            log() << "Auth schema version is incompatible: "
                  << "User and role management commands require auth data to have "
                  << "at least schema version " << AuthorizationManager::schemaVersion26Final
                  << " but startup could not verify schema version: " << status.toString() << endl;
            exitCleanly(EXIT_NEED_UPGRADE);
        }
        if (foundSchemaVersion < AuthorizationManager::schemaVersion26Final) {
            log() << "Auth schema version is incompatible: "
                  << "User and role management commands require auth data to have "
                  << "at least schema version " << AuthorizationManager::schemaVersion26Final
                  << " but found " << foundSchemaVersion << ". In order to upgrade "
                  << "the auth schema, first downgrade MongoDB binaries to version "
                  << "2.6 and then run the authSchemaUpgrade command." << endl;
            exitCleanly(EXIT_NEED_UPGRADE);
        }

        if (!storageGlobalParams.readOnly) {
            getDeleter()->startWorkers();

            restartInProgressIndexesFromLastShutdown(startupOpCtx.get());

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
        }
    }

    startClientCursorMonitor();

    PeriodicTask::startRunningPeriodicTasks();

    HostnameCanonicalizationWorker::start(getGlobalServiceContext());

    if (!storageGlobalParams.readOnly) {
        startFTDC();
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            uassertStatusOK(ShardingState::get(startupOpCtx.get())
                                ->initializeFromShardIdentity(startupOpCtx.get()));

            // Note: For replica sets, ShardingStateRecovery happens on transition to primary.
            if (!repl::getGlobalReplicationCoordinator()->isReplEnabled()) {
                uassertStatusOK(ShardingStateRecovery::recover(startupOpCtx.get()));
            }
        } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            uassertStatusOK(
                initializeGlobalShardingStateForMongod(startupOpCtx.get(),
                                                       ConnectionString::forLocal(),
                                                       kDistLockProcessIdForConfigServer));
            Balancer::create(startupOpCtx->getServiceContext());
        }

        logStartup(startupOpCtx.get());
    } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        auto parseStatus = ShardIdentityType::fromBSON(serverGlobalParams.overrideShardIdentity);
        uassertStatusOK(parseStatus);
        uassertStatusOK(ShardingState::get(startupOpCtx.get())
                            ->initializeFromShardIdentity(
                                startupOpCtx.get(), parseStatus.getValue(), Date_t::max()));
        uassertStatusOK(reloadShardRegistryUntilSuccess(startupOpCtx.get()));
    }

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    auto start = getGlobalServiceContext()->addAndStartTransportLayer(std::move(transportLayer));
    if (!start.isOK()) {
        error() << "Failed to start the listener: " << start.toString();
        return EXIT_NET_ERROR;
    }

    return waitForShutdown();
}

ExitCode initAndListen(int listenPort) {
    try {
        return _initAndListen(listenPort);
    } catch (DBException& e) {
        log() << "exception in initAndListen: " << e.toString() << ", terminating" << endl;
        return EXIT_UNCAUGHT;
    } catch (std::exception& e) {
        log() << "exception in initAndListen std::exception: " << e.what() << ", terminating";
        return EXIT_UNCAUGHT;
    } catch (int& n) {
        log() << "exception in initAndListen int: " << n << ", terminating" << endl;
        return EXIT_UNCAUGHT;
    } catch (...) {
        log() << "exception in initAndListen, terminating" << endl;
        return EXIT_UNCAUGHT;
    }
}

#if defined(_WIN32)
ExitCode initService() {
    ntservice::reportStatus(SERVICE_RUNNING);
    log() << "Service running" << endl;
    return initAndListen(serverGlobalParams.port);
}
#endif

}  // namespace mongo

using namespace mongo;

static int mongoDbMain(int argc, char* argv[], char** envp);

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables mongoDbMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = mongoDbMain(argc, wcl.argv(), wcl.envp());
    quickExit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = mongoDbMain(argc, argv, envp);
    quickExit(exitCode);
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

MONGO_INITIALIZER_WITH_PREREQUISITES(CreateReplicationManager,
                                     ("SetGlobalEnvironment", "SSLManager"))
(InitializerContext* context) {
    auto serviceContext = getGlobalServiceContext();
    repl::StorageInterface::set(serviceContext, stdx::make_unique<repl::StorageInterfaceImpl>());
    auto storageInterface = repl::StorageInterface::get(serviceContext);

    repl::TopologyCoordinatorImpl::Options topoCoordOptions;
    topoCoordOptions.maxSyncSourceLagSecs = Seconds(repl::maxSyncSourceLagSecs);
    topoCoordOptions.clusterRole = serverGlobalParams.clusterRole;

    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorImpl>(
        getGlobalReplSettings(),
        new repl::ReplicationCoordinatorExternalStateImpl(storageInterface),
        executor::makeNetworkInterface("NetworkInterfaceASIO-Replication").release(),
        new repl::TopologyCoordinatorImpl(topoCoordOptions),
        storageInterface,
        static_cast<int64_t>(curTimeMillis64()));
    serviceContext->registerKillOpListener(replCoord.get());
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

#if defined(_WIN32)
namespace mongo {
// the hook for mongoAbort
extern void (*reportEventToSystem)(const char* msg);
static void reportEventToSystemImpl(const char* msg) {
    static ::HANDLE hEventLog = RegisterEventSource(NULL, TEXT("mongod"));
    if (hEventLog) {
        std::wstring s = toNativeString(msg);
        LPCTSTR txt = s.c_str();
        BOOL ok = ReportEvent(hEventLog, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0, &txt, 0);
        wassert(ok);
    }
}
}  // namespace mongo
#endif  // if defined(_WIN32)

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
    OperationContext* txn = client.getOperationContext();
    if (!txn && serviceContext->getGlobalStorageEngine()) {
        uniqueTxn = client.makeOperationContext();
        txn = uniqueTxn.get();
    }

    getGlobalServiceContext()->getTransportLayer()->shutdown();
    log(LogComponent::kNetwork) << "shutdown: going to close listening sockets..." << endl;
    ListeningSockets::get()->closeAll();

    log(LogComponent::kNetwork) << "shutdown: going to flush diaglog..." << endl;
    _diaglog.flush();

    if (serviceContext)
        serviceContext->setKillAllOperations();

#if __has_feature(address_sanitizer)

    // When running under address sanitizer, we get false positive leaks due to disorder around the
    // lifecycle of a connection and request. When we are running under ASAN, we try a lot harder to
    // dry up the server from active connections before going on to really shut down.

    log(LogComponent::kNetwork) << "shutdown: going to close all sockets because ASAN is active...";

    // Close all sockets in a detached thread, and then wait for the number of active connections to
    // reach zero. Give the detached background thread a 10 second deadline. If we haven't closed
    // drained all active operations within that deadline, just keep going with shutdown: the OS
    // will do it for us when the process terminates.

    stdx::packaged_task<void()> dryOutTask([] {
        // There isn't currently a way to wait on the TicketHolder to have all its tickets back,
        // unfortunately. So, busy wait in this detached thread.
        while (true) {
            const auto connected = Listener::globalTicketHolder.used();
            if (connected == 0) {
                log(LogComponent::kNetwork) << "shutdown: no active connections found...";
                break;
            }
            log(LogComponent::kNetwork) << "shutdown: still waiting on " << connected
                                        << " active connections to drain... ";
            mongo::sleepFor(Milliseconds(250));
        }
    });

    auto dryNotification = dryOutTask.get_future();
    stdx::thread(std::move(dryOutTask)).detach();
    if (dryNotification.wait_for(Seconds(10).toSystemDuration()) != stdx::future_status::ready) {
        log(LogComponent::kNetwork) << "shutdown: exhausted grace period for"
                                    << " active connections to drain; continuing with shutdown... ";
    }

#endif

    // Shutdown Full-Time Data Capture
    stopFTDC();

    if (txn) {
        repl::ReplicationCoordinator::get(txn)->shutdown(txn);
        ShardingState::get(txn)->shutDown(txn);
    }

    // We should always be able to acquire the global lock at shutdown.
    //
    // TODO: This call chain uses the locker directly, because we do not want to start an
    // operation context, which also instantiates a recovery unit. Also, using the
    // lockGlobalBegin/lockGlobalComplete sequence, we avoid taking the flush lock. This will
    // all go away if we start acquiring the global/flush lock as part of ScopedTransaction.
    //
    // For a Windows service, dbexit does not call exit(), so we must leak the lock outside
    // of this function to prevent any operations from running that need a lock.
    //
    DefaultLockerImpl* globalLocker = new DefaultLockerImpl();
    LockResult result = globalLocker->lockGlobalBegin(MODE_X);
    if (result == LOCK_WAITING) {
        result = globalLocker->lockGlobalComplete(UINT_MAX);
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

static int mongoDbMain(int argc, char* argv[], char** envp) {
    static StaticObserver staticObserver;

    registerShutdownTask(shutdownTask);

#if defined(_WIN32)
    mongo::reportEventToSystem = &mongo::reportEventToSystemImpl;
#endif

    setupSignalHandlers();

    dbExecCommand = argv[0];

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
