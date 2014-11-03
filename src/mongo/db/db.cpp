// @file db.cpp : Defines main() for the mongod program.

/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <boost/thread/thread.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <limits>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_d.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/global_environment_d.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/restapi.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/snapshots.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/ttl.h"
#include "mongo/platform/process_id.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version_reporting.h"

#if !defined(_WIN32)
# include <sys/file.h>
#endif

namespace mongo {

    using logger::LogComponent;

    void (*snmpInit)() = NULL;

    extern int diagLogging;

#ifdef _WIN32
    ntservice::NtServiceDefaultStrings defaultServiceStrings = {
        L"MongoDB",
        L"MongoDB",
        L"MongoDB Server"
    };
#endif

    Timer startupSrandTimer;

    QueryResult::View emptyMoreResult(long long);


    /* todo: make this a real test.  the stuff in dbtests/ seem to do all dbdirectclient which exhaust doesn't support yet. */
// QueryOption_Exhaust
#define TESTEXHAUST 0
#if( TESTEXHAUST )
    void testExhaust() {
        sleepsecs(1);
        unsigned n = 0;
        auto f = [&n](const BSONObj& o) {
            verify( o.valid() );
            //cout << o << endl;
            n++;
            bool testClosingSocketOnError = false;
            if( testClosingSocketOnError )
                verify(false);
        };
        DBClientConnection db(false);
        db.connect("localhost");
        const char *ns = "local.foo";
        if( db.count(ns) < 10000 )
            for( int i = 0; i < 20000; i++ )
                db.insert(ns, BSON("aaa" << 3 << "b" << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));

        try {
            db.query(f, ns, Query() );
        }
        catch(...) {
            cout << "hmmm" << endl;
        }

        try {
            db.query(f, ns, Query() );
        }
        catch(...) {
            cout << "caught" << endl;
        }

        cout << n << endl;
    };
#endif

    class MyMessageHandler : public MessageHandler {
    public:
        virtual void connected( AbstractMessagingPort* p ) {
            Client::initThread("conn", p);
        }

        virtual void process( Message& m , AbstractMessagingPort* port , LastError * le) {
            boost::scoped_ptr<OperationContext> txn(new OperationContextImpl());
            while ( true ) {
                if ( inShutdown() ) {
                    log() << "got request after shutdown()" << endl;
                    break;
                }

                lastError.startRequest( m , le );

                DbResponse dbresponse;
                assembleResponse( txn.get(), m, dbresponse, port->remote() );

                if ( dbresponse.response ) {
                    port->reply(m, *dbresponse.response, dbresponse.responseTo);
                    if( dbresponse.exhaustNS.size() > 0 ) {
                        MsgData::View header = dbresponse.response->header();
                        QueryResult::View qr = header.view2ptr();
                        long long cursorid = qr.getCursorId();
                        if( cursorid ) {
                            verify( dbresponse.exhaustNS.size() && dbresponse.exhaustNS[0] );
                            string ns = dbresponse.exhaustNS; // before reset() free's it...
                            m.reset();
                            BufBuilder b(512);
                            b.appendNum((int) 0 /*size set later in appendData()*/);
                            b.appendNum(header.getId());
                            b.appendNum(header.getResponseTo());
                            b.appendNum((int) dbGetMore);
                            b.appendNum((int) 0);
                            b.appendStr(ns);
                            b.appendNum((int) 0); // ntoreturn
                            b.appendNum(cursorid);
                            m.appendData(b.buf(), b.len());
                            b.decouple();
                            DEV log() << "exhaust=true sending more" << endl;
                            continue; // this goes back to top loop
                        }
                    }
                }
                break;
            }
        }

        virtual void disconnected( AbstractMessagingPort* p ) {
            Client * c = currentClient.get();
            if( c ) c->shutdown();
        }

    };

    static void logStartup() {
        BSONObjBuilder toLog;
        stringstream id;
        id << getHostNameCached() << "-" << jsTime().asInt64();
        toLog.append( "_id", id.str() );
        toLog.append( "hostname", getHostNameCached() );

        toLog.appendTimeT( "startTime", time(0) );
        toLog.append( "startTimeLocal", dateToCtimeString(curTimeMillis64()) );

        toLog.append("cmdLine", serverGlobalParams.parsedOpts);
        toLog.append( "pid", ProcessId::getCurrent().asLongLong() );


        BSONObjBuilder buildinfo( toLog.subobjStart("buildinfo"));
        appendBuildInfo(buildinfo);
        buildinfo.doneFast();

        BSONObj o = toLog.obj();

        OperationContextImpl txn;

        Lock::GlobalWrite lk(txn.lockState());
        //  No WriteUnitOfWork, as DirectClient creates its own units of work
        DBDirectClient c(&txn);

        static const char* name = "local.startup_log";
        c.createCollection( name, 10 * 1024 * 1024, true );
        c.insert( name, o);
    }

    static void checkForIdIndexes(OperationContext* txn, Database* db) {
        if ( db->name() == "local") {
            // we do not need an _id index on anything in the local database
            return;
        }

        list<string> collections;
        db->getDatabaseCatalogEntry()->getCollectionNamespaces( &collections );

        // for each collection, ensure there is a $_id_ index
        for (list<string>::iterator i = collections.begin(); i != collections.end(); ++i) {
            const string& collectionName = *i;
            NamespaceString ns( collectionName );
            if ( ns.isSystem() )
                continue;

            Collection* coll = db->getCollection( txn, collectionName );
            if ( !coll )
                continue;

            if ( coll->getIndexCatalog()->findIdIndex( txn ) )
                continue;

            log() << "WARNING: the collection '" << *i
                  << "' lacks a unique index on _id."
                  << " This index is needed for replication to function properly"
                  << startupWarningsLog;
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
        Lock::GlobalWrite lk(txn->lockState());
        if (!repl::getGlobalReplicationCoordinator()->getSettings().usingReplSets()) {
            DBDirectClient c(txn);
            return c.count("local.system.replset");
        }
        return 0;
    }

    static void repairDatabasesAndCheckVersion() {
        LOG(1) << "enter repairDatabases (to check pdfile version #)" << endl;

        OperationContextImpl txn;
        Lock::GlobalWrite lk(txn.lockState());

        vector<string> dbNames;

        StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
        storageEngine->listDatabases( &dbNames );

        // Repair all databases first, so that we do not try to open them if they are in bad shape
        if (storageGlobalParams.repair) {
            for (vector<string>::const_iterator i = dbNames.begin(); i != dbNames.end(); ++i) {
                const string dbName = *i;
                LOG(1) << "    Repairing database: " << dbName << endl;

                fassert(18506, storageEngine->repairDatabase(&txn, dbName));
            }
        }

        const repl::ReplSettings& replSettings =
            repl::getGlobalReplicationCoordinator()->getSettings();

        // On replica set members we only clear temp collections on DBs other than "local" during
        // promotion to primary. On pure slaves, they are only cleared when the oplog tells them
        // to. The local DB is special because it is not replicated.  See SERVER-10927 for more
        // details.
        const bool shouldClearNonLocalTmpCollections = !(checkIfReplMissingFromCommandLine(&txn)
                                                    || replSettings.usingReplSets()
                                                    || replSettings.slave == repl::SimpleSlave);

        for (vector<string>::const_iterator i = dbNames.begin(); i != dbNames.end(); ++i) {
            const string dbName = *i;
            LOG(1) << "    Recovering database: " << dbName << endl;

            Database* db = dbHolder().openDb(&txn, dbName);
            invariant(db);

            // First thing after opening the database is to check for file compatibility,
            // otherwise we might crash if this is a deprecated format.
            if (!db->getDatabaseCatalogEntry()->currentFilesCompatible(&txn)) {
                log() << "****";
                log() << "cannot do this upgrade without an upgrade in the middle";
                log() << "please do a --repair with 2.6 and then start this version";
                dbexit(EXIT_NEED_UPGRADE);
                return;
            }

            // Major versions match, check indexes
            const string systemIndexes = db->name() + ".system.indexes";

            Collection* coll = db->getCollection( &txn, systemIndexes );
            auto_ptr<PlanExecutor> exec(
                InternalPlanner::collectionScan(&txn, systemIndexes, coll));

            BSONObj index;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&index, NULL))) {
                const BSONObj key = index.getObjectField("key");
                const string plugin = IndexNames::findPluginName(key);

                if (db->getDatabaseCatalogEntry()->isOlderThan24(&txn)) {
                    if (IndexNames::existedBefore24(plugin)) {
                        continue;
                    }

                    log() << "Index " << index << " claims to be of type '" << plugin << "', "
                            << "which is either invalid or did not exist before v2.4. "
                            << "See the upgrade section: "
                            << "http://dochub.mongodb.org/core/upgrade-2.4"
                            << startupWarningsLog;
                }

                const Status keyStatus = validateKeyPattern(key);
                if (!keyStatus.isOK()) {
                    log() << "Problem with index " << index << ": " << keyStatus.reason()
                            << " This index can still be used however it cannot be rebuilt."
                            << " For more info see"
                            << " http://dochub.mongodb.org/core/index-validation"
                            << startupWarningsLog;
                }
            }

            if (PlanExecutor::IS_EOF != state) {
                warning() << "Internal error while reading collection " << systemIndexes;
            }

            if (replSettings.usingReplSets()) {
                // We only care about the _id index if we are in a replset
                checkForIdIndexes(&txn, db);
            }

            if (shouldClearNonLocalTmpCollections || dbName == "local") {
                db->clearTmpCollections(&txn);
            }
        }

        LOG(1) << "done repairDatabases" << endl;
    }

    static void _initAndListen(int listenPort ) {
        Client::initThread("initandlisten");

        const repl::ReplSettings& replSettings =
                repl::getGlobalReplicationCoordinator()->getSettings();

        {
            ProcessId pid = ProcessId::getCurrent();
            LogstreamBuilder l = log(LogComponent::kControl);
            l << "MongoDB starting : pid=" << pid
              << " port=" << serverGlobalParams.port
              << " dbpath=" << storageGlobalParams.dbpath;
            if( replSettings.master ) l << " master=" << replSettings.master;
            if( replSettings.slave )  l << " slave=" << (int) replSettings.slave;

            const bool is32bit = sizeof(int*) == 4;
            l << ( is32bit ? " 32" : " 64" ) << "-bit host=" << getHostNameCached() << endl;
        }

        DEV log(LogComponent::kControl) << "_DEBUG build (which is slower)" << endl;
        logMongodStartupWarnings();

#if defined(_WIN32)
        printTargetMinOS();
#endif

        logProcessDetails();

        {
            stringstream ss;
            ss << endl;
            ss << "*********************************************************************" << endl;
            ss << " ERROR: dbpath (" << storageGlobalParams.dbpath << ") does not exist." << endl;
            ss << " Create this directory or give existing directory in --dbpath." << endl;
            ss << " See http://dochub.mongodb.org/core/startingandstoppingmongo" << endl;
            ss << "*********************************************************************" << endl;
            uassert(10296,  ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.dbpath));
        }

        {
            stringstream ss;
            ss << "repairpath (" << storageGlobalParams.repairpath << ") does not exist";
            uassert(12590, 
                    ss.str().c_str(),
                    boost::filesystem::exists(storageGlobalParams.repairpath));
        }

        // Disallow configurations for multiple registered storage engines in
        // the same configuration file/environment.
        {
            BSONElement storageElement = serverGlobalParams.parsedOpts.getField("storage");
            invariant(storageElement.isABSONObj());
            BSONObj storageParamsObj = storageElement.Obj();
            BSONObjIterator i = storageParamsObj.begin();
            while (i.more()) {
                BSONElement e = i.next();
                // Ignore if field name under "storage" matches current storage engine.
                if (storageGlobalParams.engine == e.fieldName()) continue;
                // Raise an error if field name matches non-active registered storage engine.
                if (getGlobalEnvironment()->isRegisteredStorageEngine(e.fieldName())) {
                    uasserted(28547, str::stream()
                        << "Detected configuration for non-active storage engine " << e.fieldName()
                        << " when current storage engine is " << storageGlobalParams.engine);
                }
            }
        }

        // Due to SERVER-15389, we must setupSockets first thing at startup in order to avoid
        // obtaining too high a file descriptor for our calls to select().
        MessageServer::Options options;
        options.port = listenPort;
        options.ipList = serverGlobalParams.bind_ip;

        MessageServer* server = createServer(options, new MyMessageHandler());
        server->setAsTimeTracker();

        // This is what actually creates the sockets, but does not yet listen on them because we
        // do not want connections to just hang if recovery takes a very long time.
        server->setupSockets();

        // TODO:  This should go into a MONGO_INITIALIZER once we have figured out the correct
        // dependencies.
        if (snmpInit) {
            snmpInit();
        }

        getGlobalEnvironment()->setGlobalStorageEngine(storageGlobalParams.engine);

        boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");

        if (mmapv1GlobalOptions.journalOptions & MMAPV1Options::JournalRecoverOnly)
            return;

        if (mongodGlobalParams.scriptingEnabled) {
            ScriptEngine::setup();
        }

        repairDatabasesAndCheckVersion();

        if (storageGlobalParams.upgrade) {
            log() << "finished checking dbs" << endl;
            cc().shutdown();
            exitCleanly(EXIT_CLEAN);
        }

        {
            OperationContextImpl txn;
            uassertStatusOK(getGlobalAuthorizationManager()->initialize(&txn));
        }

        /* this is for security on certain platforms (nonce generation) */
        srand((unsigned) (curTimeMicros() ^ startupSrandTimer.micros()));

        // The snapshot thread provides historical collection level and lock statistics for use
        // by the web interface. Only needed when HTTP is enabled.
        if (serverGlobalParams.isHttpInterfaceEnabled) {
            snapshotThread.go();

            boost::thread web(stdx::bind(&webServerThread, new RestAdminAccess()));
            web.detach();
        }

        startClientCursorMonitor();

        PeriodicTask::startRunningPeriodicTasks();

        {
            OperationContextImpl txn;

            const unsigned long long missingRepl = checkIfReplMissingFromCommandLine(&txn);
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
            }
            else {
                startTTLBackgroundJob();
            }

#ifndef _WIN32
        mongo::signalForkSuccess();
#endif

            authindex::configureSystemIndexes(&txn);

            // SERVER-14090: Verify that auth schema version is schemaVersion26Final.
            int foundSchemaVersion;
            Status status = getGlobalAuthorizationManager()->getAuthorizationVersion(
                    &txn, &foundSchemaVersion);
            if (!status.isOK()) {
                log() << "Auth schema version is incompatible: "
                      << "User and role management commands require auth data to have "
                      << "at least schema version " << AuthorizationManager::schemaVersion26Final
                      << " but startup could not verify schema version: " << status.toString()
                      << endl;
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

            getDeleter()->startWorkers();

            restartInProgressIndexesFromLastShutdown(&txn);

            repl::getGlobalReplicationCoordinator()->startReplication(&txn);
        }

        logStartup();

#if(TESTEXHAUST)
        boost::thread thr(testExhaust);
#endif

        // MessageServer::run will return when exit code closes its socket
        server->run();
    }

    ExitCode initAndListen(int listenPort) {
        try {
            _initAndListen(listenPort);

            return inShutdown() ? EXIT_CLEAN : EXIT_NET_ERROR;
        }
        catch ( DBException &e ) {
            log() << "exception in initAndListen: " << e.toString() << ", terminating" << endl;
            return EXIT_UNCAUGHT;
        }
        catch ( std::exception &e ) {
            log() << "exception in initAndListen std::exception: " << e.what() << ", terminating";
            return EXIT_UNCAUGHT;
        }
        catch ( int& n ) {
            log() << "exception in initAndListen int: " << n << ", terminating" << endl;
            return EXIT_UNCAUGHT;
        }
        catch(...) {
            log() << "exception in initAndListen, terminating" << endl;
            return EXIT_UNCAUGHT;
        }
    }

#if defined(_WIN32)
    ExitCode initService() {
        ntservice::reportStatus( SERVICE_RUNNING );
        log() << "Service running" << endl;
        return initAndListen(serverGlobalParams.port);
    }
#endif

} // namespace mongo

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

MONGO_INITIALIZER_GENERAL(ForkServer,
                          ("EndStartupOptionHandling"),
                          ("default"))(InitializerContext* context) {
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
        vector<string> command = moe::startupOptionsParsed["command"].as< vector<string> >();

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

        string name = (boost::filesystem::path(storageGlobalParams.dbpath) / "mongod.lock").string();
        if ( !boost::filesystem::exists( name ) || boost::filesystem::file_size( name ) == 0 )
            failed = true;

        pid_t pid;
        string procPath;
        if (!failed){
            try {
                ifstream f (name.c_str());
                f >> pid;
                procPath = (str::stream() << "/proc/" << pid);
                if (!boost::filesystem::exists(procPath))
                    failed = true;
            }
            catch (const std::exception& e){
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

MONGO_INITIALIZER_GENERAL(CreateAuthorizationManager,
                          ("SetupInternalSecurityUser", "OIDGeneration"),
                          MONGO_NO_DEPENDENTS)
        (InitializerContext* context) {
    AuthorizationManager* authzManager =
            new AuthorizationManager(new AuthzManagerExternalStateMongod());
    setGlobalAuthorizationManager(authzManager);
    return Status::OK();
}

#ifdef MONGO_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, 
                          MONGO_NO_PREREQUISITES, 
                          ("SSLManager"))(InitializerContext* context) {
    isSSLServer = true;
    return Status::OK();
}
#endif

#if defined(_WIN32)
namespace mongo {
    // the hook for mongoAbort
    extern void (*reportEventToSystem)(const char *msg);
    static void reportEventToSystemImpl(const char *msg) {
        static ::HANDLE hEventLog = RegisterEventSource( NULL, TEXT("mongod") );
        if( hEventLog ) {
            std::wstring s = toNativeString(msg);
            LPCTSTR txt = s.c_str();
            BOOL ok = ReportEvent(
              hEventLog, EVENTLOG_ERROR_TYPE,
              0, 0, NULL,
              1,
              0,
              &txt,
              0);
            wassert(ok);
        }
    }
} // namespace mongo
#endif  // if defined(_WIN32)

static int mongoDbMain(int argc, char* argv[], char **envp) {
    static StaticObserver staticObserver;

#if defined(_WIN32)
    mongo::reportEventToSystem = &mongo::reportEventToSystemImpl;
#endif

    setupSignalHandlers(false);

    dbExecCommand = argv[0];

    srand(curTimeMicros());

    {
        unsigned x = 0x12345678;
        unsigned char& b = (unsigned char&) x;
        if ( b != 0x78 ) {
            mongo::log(LogComponent::kControl) << "big endian cpus not yet supported" << endl;
            return 33;
        }
    }

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
