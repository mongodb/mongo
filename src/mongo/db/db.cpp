// @file db.cpp : Defines main() for the mongod program.

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include <boost/thread/thread.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/auth/authz_manager_external_state_d.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/d_globals.h"
#include "mongo/db/db.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/dur.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/repl_start.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/restapi.h"
#include "mongo/db/startup_warnings.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/snapshots.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/ttl.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/d_writeback.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/background.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_win32.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"
#include "mongo/util/version_reporting.h"

#if !defined(_WIN32)
# include <sys/file.h>
#endif

namespace mongo {

    void (*snmpInit)() = NULL;

    /* only off if --nohints */
    extern bool useHints;

    extern int diagLogging;
    extern int lockFile;

    static void setupSignalHandlers();
    static void startSignalProcessingThread();
    void exitCleanly( ExitCode code );

#ifdef _WIN32
    ntservice::NtServiceDefaultStrings defaultServiceStrings = {
        L"MongoDB",
        L"Mongo DB",
        L"Mongo DB Server"
    };
#endif

    Timer startupSrandTimer;

    const char *ourgetns() {
        Client *c = currentClient.get();
        if ( ! c )
            return "";
        Client::Context* cc = c->getContext();
        return cc ? cc->ns() : "";
    }

    struct MyStartupTests {
        MyStartupTests() {
            verify( sizeof(OID) == 12 );
        }
    } mystartupdbcpp;

    QueryResult* emptyMoreResult(long long);


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

    /* if server is really busy, wait a bit */
    void beNice() {
        sleepmicros( Client::recommendedYieldMicros() );
    }

    class MyMessageHandler : public MessageHandler {
    public:
        virtual void connected( AbstractMessagingPort* p ) {
            Client::initThread("conn", p);
        }

        virtual void process( Message& m , AbstractMessagingPort* port , LastError * le) {
            while ( true ) {
                if ( inShutdown() ) {
                    log() << "got request after shutdown()" << endl;
                    break;
                }

                lastError.startRequest( m , le );

                DbResponse dbresponse;
                try {
                    assembleResponse( m, dbresponse, port->remote() );
                }
                catch ( const ClockSkewException & ) {
                    log() << "ClockSkewException - shutting down" << endl;
                    exitCleanly( EXIT_CLOCK_SKEW );
                }

                if ( dbresponse.response ) {
                    port->reply(m, *dbresponse.response, dbresponse.responseTo);
                    if( dbresponse.exhaustNS.size() > 0 ) {
                        MsgData *header = dbresponse.response->header();
                        QueryResult *qr = (QueryResult *) header;
                        long long cursorid = qr->cursorId;
                        if( cursorid ) {
                            verify( dbresponse.exhaustNS.size() && dbresponse.exhaustNS[0] );
                            string ns = dbresponse.exhaustNS; // before reset() free's it...
                            m.reset();
                            BufBuilder b(512);
                            b.appendNum((int) 0 /*size set later in appendData()*/);
                            b.appendNum(header->id);
                            b.appendNum(header->responseTo);
                            b.appendNum((int) dbGetMore);
                            b.appendNum((int) 0);
                            b.appendStr(ns);
                            b.appendNum((int) 0); // ntoreturn
                            b.appendNum(cursorid);
                            m.appendData(b.buf(), b.len());
                            b.decouple();
                            DEV log() << "exhaust=true sending more" << endl;
                            beNice();
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

    void logStartup() {
        BSONObjBuilder toLog;
        stringstream id;
        id << getHostNameCached() << "-" << jsTime();
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

        Lock::GlobalWrite lk;
        DBDirectClient c;
        const char* name = "local.startup_log";
        c.createCollection( name, 10 * 1024 * 1024, true );
        c.insert( name, o);
    }

    void listen(int port) {
        //testTheDb();
        MessageServer::Options options;
        options.port = port;
        options.ipList = serverGlobalParams.bind_ip;

        MessageServer * server = createServer( options , new MyMessageHandler() );
        server->setAsTimeTracker();
        // we must setupSockets prior to logStartup() to avoid getting too high
        // a file descriptor for our calls to select()
        server->setupSockets();

        logStartup();
        startReplication();
        if (serverGlobalParams.isHttpInterfaceEnabled)
            boost::thread web( boost::bind(&webServerThread, new RestAdminAccess() /* takes ownership */));

#if(TESTEXHAUST)
        boost::thread thr(testExhaust);
#endif
        server->run();
    }


    void doDBUpgrade( const string& dbName, DataFileHeader* h ) {
        static DBDirectClient db;

        if ( h->version == 4 && h->versionMinor == 4 ) {
            verify( PDFILE_VERSION == 4 );
            verify( PDFILE_VERSION_MINOR_22_AND_OLDER == 5 );

            list<string> colls = db.getCollectionNames( dbName );
            for ( list<string>::iterator i=colls.begin(); i!=colls.end(); i++) {
                string c = *i;
                log() << "\t upgrading collection:" << c << endl;
                BSONObj out;
                bool ok = db.runCommand( dbName , BSON( "reIndex" << c.substr( dbName.size() + 1 ) ) , out );
                if ( ! ok ) {
                    log() << "\t\t reindex failed: " << out;
                    fassertFailed( 17393 );
                }
            }

            getDur().writingInt(h->versionMinor) = 5;
            return;
        }

        // do this in the general case
        fassert( 17401, repairDatabase( dbName ) );
    }

    void checkForIdIndexes( Database* db ) {

        if ( db->name() == "local") {
            // we do not need an _id index on anything in the local database
            return;
        }

        list<string> collections;
        db->namespaceIndex().getNamespaces( collections );

        // for each collection, ensure there is a $_id_ index
        for (list<string>::iterator i = collections.begin(); i != collections.end(); ++i) {
            const string& collectionName = *i;
            NamespaceString ns( collectionName );
            if ( ns.isSystem() )
                continue;

            Collection* coll = db->getCollection( collectionName );
            if ( !coll )
                continue;

            if ( coll->getIndexCatalog()->findIdIndex() )
                continue;

            log() << "WARNING: the collection '" << *i
                  << "' lacks a unique index on _id."
                  << " This index is needed for replication to function properly"
                  << startupWarningsLog;
            log() << "\t To fix this, on the primary run 'db." << i->substr(i->find('.')+1)
                  << ".createIndex({_id: 1}, {unique: true})'"
                  << startupWarningsLog;
        }
    }

    // ran at startup.
    static void repairDatabasesAndCheckVersion(bool shouldClearNonLocalTmpCollections) {
        //        LastError * le = lastError.get( true );
        LOG(1) << "enter repairDatabases (to check pdfile version #)" << endl;

        Lock::GlobalWrite lk;
        vector< string > dbNames;
        getDatabaseNames( dbNames );
        for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
            string dbName = *i;
            LOG(1) << "\t" << dbName << endl;

            Client::Context ctx( dbName );
            DataFile *p = ctx.db()->getExtentManager().getFile( 0 );
            DataFileHeader *h = p->getHeader();

            if ( replSettings.usingReplSets() ) {
                // we only care about the _id index if we are in a replset
                checkForIdIndexes(ctx.db());
            }

            if (shouldClearNonLocalTmpCollections || dbName == "local")
                ctx.db()->clearTmpCollections();

            if (!h->isCurrentVersion() || mongodGlobalParams.repair) {

                if( h->version <= 0 ) {
                    uasserted(14026,
                      str::stream() << "db " << dbName << " appears corrupt pdfile version: " << h->version
                                    << " info: " << h->versionMinor << ' ' << h->fileLength);
                }

                if ( !h->isCurrentVersion() ) {
                    log() << "****" << endl;
                    log() << "****" << endl;
                    log() << "need to upgrade database " << dbName << " "
                          << "with pdfile version " << h->version << "." << h->versionMinor << ", "
                          << "new version: "
                          << PDFILE_VERSION << "." << PDFILE_VERSION_MINOR_22_AND_OLDER
                          << endl;
                }

                if (mongodGlobalParams.upgrade) {
                    // QUESTION: Repair even if file format is higher version than code?
                    doDBUpgrade( dbName, h );
                }
                else {
                    log() << "\t Not upgrading, exiting" << endl;
                    log() << "\t run --upgrade to upgrade dbs, then start again" << endl;
                    log() << "****" << endl;
                    dbexit( EXIT_NEED_UPGRADE );
                    mongodGlobalParams.upgrade = 1;
                    return;
                }
            }
            else {
                const string systemIndexes = cc().database()->name() + ".system.indexes";
                auto_ptr<Runner> runner(InternalPlanner::collectionScan(systemIndexes));
                BSONObj index;
                Runner::RunnerState state;
                while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&index, NULL))) {
                    const BSONObj key = index.getObjectField("key");
                    const string plugin = IndexNames::findPluginName(key);

                    if (h->versionMinor == PDFILE_VERSION_MINOR_22_AND_OLDER) {
                        if (IndexNames::existedBefore24(plugin))
                            continue;

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

                if (Runner::RUNNER_EOF != state) {
                    warning() << "Internal error while reading collection " << systemIndexes;
                }

                Database::closeDatabase(dbName.c_str(), storageGlobalParams.dbpath);
            }
        }

        LOG(1) << "done repairDatabases" << endl;

        if (mongodGlobalParams.upgrade) {
            log() << "finished checking dbs" << endl;
            cc().shutdown();
            dbexit( EXIT_CLEAN );
        }
    }

    void clearTmpFiles() {
        boost::filesystem::path path(storageGlobalParams.dbpath);
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            string fileName = boost::filesystem::path(*i).leaf().string();
            if ( boost::filesystem::is_directory( *i ) &&
                    fileName.length() && fileName[ 0 ] == '$' )
                boost::filesystem::remove_all( *i );
        }
    }

    /**
     * Checks if this server was started without --replset but has a config in local.system.replset
     * (meaning that this is probably a replica set member started in stand-alone mode).
     *
     * @returns the number of documents in local.system.replset or 0 if this was started with
     *          --replset.
     */
    unsigned long long checkIfReplMissingFromCommandLine() {
        Lock::GlobalWrite lk; // this is helpful for the query below to work as you can't open files when readlocked
        if (!replSettings.usingReplSets()) {
            DBDirectClient c;
            return c.count("local.system.replset");
        }
        return 0;
    }

    /**
     * does background async flushes of mmapped files
     */
    class DataFileSync : public BackgroundJob , public ServerStatusSection {
    public:
        DataFileSync()
            : ServerStatusSection( "backgroundFlushing" ),
              _total_time( 0 ),
              _flushes( 0 ),
              _last() {
        }

        virtual bool includeByDefault() const { return true; }

        string name() const { return "DataFileSync"; }

        void run() {
            Client::initThread( name().c_str() );
            if (storageGlobalParams.syncdelay == 0) {
                log() << "warning: --syncdelay 0 is not recommended and can have strange performance" << endl;
            }
            else if (storageGlobalParams.syncdelay == 1) {
                log() << "--syncdelay 1" << endl;
            }
            else if (storageGlobalParams.syncdelay != 60) {
                LOG(1) << "--syncdelay " << storageGlobalParams.syncdelay << endl;
            }
            int time_flushing = 0;
            while ( ! inShutdown() ) {
                _diaglog.flush();
                if (storageGlobalParams.syncdelay == 0) {
                    // in case at some point we add an option to change at runtime
                    sleepsecs(5);
                    continue;
                }

                sleepmillis((long long) std::max(0.0, (storageGlobalParams.syncdelay * 1000) - time_flushing));

                if ( inShutdown() ) {
                    // occasional issue trying to flush during shutdown when sleep interrupted
                    break;
                }

                Date_t start = jsTime();
                int numFiles = MemoryMappedFile::flushAll( true );
                time_flushing = (int) (jsTime() - start);

                _flushed(time_flushing);

                if( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1)) || time_flushing >= 10000 ) {
                    log() << "flushing mmaps took " << time_flushing << "ms " << " for " << numFiles << " files" << endl;
                }
            }
        }

        BSONObj generateSection(const BSONElement& configElement) const {
            BSONObjBuilder b;
            b.appendNumber( "flushes" , _flushes );
            b.appendNumber( "total_ms" , _total_time );
            b.appendNumber( "average_ms" , (_flushes ? (_total_time / double(_flushes)) : 0.0) );
            b.appendNumber( "last_ms" , _last_time );
            b.append("last_finished", _last);
            return b.obj();
        }

    private:

        void _flushed(int ms) {
            _flushes++;
            _total_time += ms;
            _last_time = ms;
            _last = jsTime();
        }

        long long _total_time;
        long long _flushes;
        int _last_time;
        Date_t _last;


    } dataFileSync;

    namespace {
        class MemJournalServerStatusMetric : public ServerStatusMetric {
        public:
            MemJournalServerStatusMetric() : ServerStatusMetric(".mem.mapped") {}
            virtual void appendAtLeaf( BSONObjBuilder& b ) const {
                int m = static_cast<int>(MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ));
                b.appendNumber( "mapped" , m );

                if (storageGlobalParams.dur) {
                    m *= 2;
                    b.appendNumber( "mappedWithJournal" , m );
                }

            }
        } memJournalServerStatusMetric;
    }


    const char * jsInterruptCallback() {
        // should be safe to interrupt in js code, even if we have a write lock
        return killCurrentOp.checkForInterruptNoAssert();
    }

    unsigned jsGetCurrentOpIdCallback() {
        return cc().curop()->opNum();
    }

    /// warn if readahead > 256KB (gridfs chunk size)
    static void checkReadAhead(const string& dir) {
#ifdef __linux__
        try {
            const dev_t dev = getPartition(dir);

            // This path handles the case where the filesystem uses the whole device (including LVM)
            string path = str::stream() <<
                "/sys/dev/block/" << major(dev) << ':' << minor(dev) << "/queue/read_ahead_kb";

            if (!boost::filesystem::exists(path)){
                // This path handles the case where the filesystem is on a partition.
                path = str::stream()
                    << "/sys/dev/block/" << major(dev) << ':' << minor(dev) // this is a symlink
                    << "/.." // parent directory of a partition is for the whole device
                    << "/queue/read_ahead_kb";
            }

            if (boost::filesystem::exists(path)) {
                ifstream file (path.c_str());
                if (file.is_open()) {
                    int kb;
                    file >> kb;
                    if (kb > 256) {
                        log() << startupWarningsLog;

                        log() << "** WARNING: Readahead for " << dir << " is set to " << kb << "KB"
                                << startupWarningsLog;

                        log() << "**          We suggest setting it to 256KB (512 sectors) or less"
                                << startupWarningsLog;

                        log() << "**          http://dochub.mongodb.org/core/readahead"
                                << startupWarningsLog;
                    }
                }
            }
        }
        catch (const std::exception& e) {
            log() << "unable to validate readahead settings due to error: " << e.what()
                  << startupWarningsLog;
            log() << "for more information, see http://dochub.mongodb.org/core/readahead"
                  << startupWarningsLog;
        }
#endif // __linux__
    }

    void _initAndListen(int listenPort ) {

        Client::initThread("initandlisten");

        bool is32bit = sizeof(int*) == 4;

        {
            ProcessId pid = ProcessId::getCurrent();
            LogstreamBuilder l = log();
            l << "MongoDB starting : pid=" << pid
              << " port=" << serverGlobalParams.port
              << " dbpath=" << storageGlobalParams.dbpath;
            if( replSettings.master ) l << " master=" << replSettings.master;
            if( replSettings.slave )  l << " slave=" << (int) replSettings.slave;
            l << ( is32bit ? " 32" : " 64" ) << "-bit host=" << getHostNameCached() << endl;
        }
        DEV log() << "_DEBUG build (which is slower)" << endl;
        logStartupWarnings();
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
            uassert(12590,  ss.str().c_str(),
                    boost::filesystem::exists(storageGlobalParams.repairpath));
        }

        // TODO check non-journal subdirs if using directory-per-db
        checkReadAhead(storageGlobalParams.dbpath);

        acquirePathLock(mongodGlobalParams.repair);
        boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");

        FileAllocator::get()->start();

        // TODO:  This should go into a MONGO_INITIALIZER once we have figured out the correct
        // dependencies.
        if (snmpInit) {
            snmpInit();
        }

        MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( clearTmpFiles(), "clear tmp files" );

        dur::startup();

        if (storageGlobalParams.durOptions & StorageGlobalParams::DurRecoverOnly)
            return;

        unsigned long long missingRepl = checkIfReplMissingFromCommandLine();
        if (missingRepl) {
            log() << startupWarningsLog;
            log() << "** WARNING: mongod started without --replSet yet " << missingRepl
                  << " documents are present in local.system.replset" << startupWarningsLog;
            log() << "**          Restart with --replSet unless you are doing maintenance and no"
                  << " other clients are connected." << startupWarningsLog;
            log() << "**          The TTL collection monitor will not start because of this." << startupWarningsLog;
            log() << "**          For more info see http://dochub.mongodb.org/core/ttlcollections" << startupWarningsLog;
            log() << startupWarningsLog;
        }

        if (mongodGlobalParams.scriptingEnabled) {
            ScriptEngine::setup();
            globalScriptEngine->setCheckInterruptCallback( jsInterruptCallback );
            globalScriptEngine->setGetCurrentOpIdCallback( jsGetCurrentOpIdCallback );
        }

        // On replica set members we only clear temp collections on DBs other than "local" during
        // promotion to primary. On pure slaves, they are only cleared when the oplog tells them to.
        // The local DB is special because it is not replicated.  See SERVER-10927 for more details.
        const bool shouldClearNonLocalTmpCollections = !(missingRepl
                                                         || replSettings.usingReplSets()
                                                         || replSettings.slave == SimpleSlave);
        repairDatabasesAndCheckVersion(shouldClearNonLocalTmpCollections);

        if (mongodGlobalParams.upgrade)
            return;

        uassertStatusOK(getGlobalAuthorizationManager()->initialize());

        /* this is for security on certain platforms (nonce generation) */
        srand((unsigned) (curTimeMicros() ^ startupSrandTimer.micros()));

        snapshotThread.go();
        d.clientCursorMonitor.go();
        PeriodicTask::startRunningPeriodicTasks();
        if (missingRepl) {
            // a warning was logged earlier
        }
        else {
            startTTLBackgroundJob();
        }

#ifndef _WIN32
        mongo::signalForkSuccess();
#endif

        if(getGlobalAuthorizationManager()->isAuthEnabled()) {
            // open admin db in case we need to use it later. TODO this is not the right way to
            // resolve this.
            Client::WriteContext c("admin", storageGlobalParams.dbpath);
        }

        authindex::configureSystemIndexes("admin");

        getDeleter()->startWorkers();

        // Starts a background thread that rebuilds all incomplete indices. 
        indexRebuilder.go(); 

        listen(listenPort);

        // listen() will return when exit code closes its socket.
        exitCleanly(EXIT_NET_ERROR);
    }

    void initAndListen(int listenPort) {
        try {
            _initAndListen(listenPort);
        }
        catch ( DBException &e ) {
            log() << "exception in initAndListen: " << e.toString() << ", terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }
        catch ( std::exception &e ) {
            log() << "exception in initAndListen std::exception: " << e.what() << ", terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }
        catch ( int& n ) {
            log() << "exception in initAndListen int: " << n << ", terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }
        catch(...) {
            log() << "exception in initAndListen, terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }
    }

#if defined(_WIN32)
    void initService() {
        ntservice::reportStatus( SERVICE_RUNNING );
        log() << "Service running" << endl;
        initAndListen(serverGlobalParams.port);
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
    ::_exit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = mongoDbMain(argc, argv, envp);
    ::_exit(exitCode);
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
            ::_exit(EXIT_SUCCESS);
        }

        if (command[0].compare("run") != 0) {
            cout << "Invalid command: " << command[0] << endl;
            printMongodHelp(moe::startupOptions);
            ::_exit(EXIT_FAILURE);
        }

        if (command.size() > 1) {
            cout << "Too many parameters to 'run' command" << endl;
            printMongodHelp(moe::startupOptions);
            ::_exit(EXIT_FAILURE);
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
    if (moe::startupOptionsParsed.count("shutdown")){
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
            ::_exit(EXIT_FAILURE);
        }

        cout << "killing process with pid: " << pid << endl;
        int ret = kill(pid, SIGTERM);
        if (ret) {
            int e = errno;
            cerr << "failed to kill process: " << errnoWithDescription(e) << endl;
            ::_exit(EXIT_FAILURE);
        }

        while (boost::filesystem::exists(procPath)) {
            sleepsecs(1);
        }

        ::_exit(EXIT_SUCCESS);
    }
#endif
}

MONGO_INITIALIZER_GENERAL(CreateAuthorizationManager,
                          ("SetupInternalSecurityUser"),
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

static int mongoDbMain(int argc, char* argv[], char **envp) {
    static StaticObserver staticObserver;

    getcurns = ourgetns;

    setupSignalHandlers();

    dbExecCommand = argv[0];

    srand(curTimeMicros());

    {
        unsigned x = 0x12345678;
        unsigned char& b = (unsigned char&) x;
        if ( b != 0x78 ) {
            out() << "big endian cpus not yet supported" << endl;
            return 33;
        }
    }

    if( argc == 1 )
        cout << dbExecCommand << " --help for help and startup options" << endl;

    Status status = mongo::runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe() << "Failed global initialization: " << status;
        ::_exit(EXIT_FAILURE);
    }

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    if (!initializeServerGlobalState())
        ::_exit(EXIT_FAILURE);

    // Per SERVER-7434, startSignalProcessingThread() must run after any forks
    // (initializeServerGlobalState()) and before creation of any other threads.
    startSignalProcessingThread();

    dataFileSync.go();

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // exits directly and so never reaches here either.
    }
#endif

    StartupTest::runTests();
    initAndListen(serverGlobalParams.port);
    dbexit(EXIT_CLEAN);
    return 0;
}

namespace mongo {

    string getDbContext();

#undef out


#if !defined(_WIN32)

} // namespace mongo

#include <signal.h>
#include <string.h>

namespace mongo {

    void abruptQuit(int x) {
        ostringstream ossSig;
        ossSig << "Got signal: " << x << " (" << strsignal( x ) << ")." << endl;
        rawOut( ossSig.str() );

        /*
        ostringstream ossOp;
        ossOp << "Last op: " << currentOp.infoNoauth() << endl;
        rawOut( ossOp.str() );
        */

        ostringstream oss;
        oss << "Backtrace:" << endl;
        printStackTrace( oss );
        rawOut( oss.str() );

        // Don't go through normal shutdown procedure. It may make things worse.
        ::_exit(EXIT_ABRUPT);

    }

    void abruptQuitWithAddrSignal( int signal, siginfo_t *siginfo, void * ) {
        ostringstream oss;
        oss << "Invalid";
        if ( signal == SIGSEGV || signal == SIGBUS ) {
            oss << " access";
        } else {
            oss << " operation";
        }
        oss << " at address: " << siginfo->si_addr << " from thread: " << getThreadName() << endl;
        rawOut( oss.str() );
        abruptQuit( signal );
    }

    sigset_t asyncSignals;
    // The signals in asyncSignals will be processed by this thread only, in order to
    // ensure the db and log mutexes aren't held.
    void signalProcessingThread() {
        while (true) {
            int actualSignal = 0;
            int status = sigwait( &asyncSignals, &actualSignal );
            fassert(16781, status == 0);
            switch (actualSignal) {
            case SIGUSR1:
                // log rotate signal
                fassert(16782, rotateLogs());
                logProcessDetailsForLogRotate();
                break;
            default:
                // interrupt/terminate signal
                Client::initThread( "signalProcessingThread" );
                log() << "got signal " << actualSignal << " (" << strsignal( actualSignal )
                      << "), will terminate after current cmd ends" << endl;
                exitCleanly( EXIT_CLEAN );
                break;
            }
        }
    }

    // this will be called in certain c++ error cases, for example if there are two active
    // exceptions
    void myterminate() {
        rawOut( "terminate() called, printing stack (if implemented for platform):" );
        printStackTrace();
        ::abort();
    }

    // this gets called when new fails to allocate memory
    void my_new_handler() {
        rawOut( "out of memory, printing stack and exiting:" );
        printStackTrace();
        ::_exit(EXIT_ABRUPT);
    }

    void setupSignals_ignoreHelper( int signal ) {}

    void setupSignalHandlers() {
        setupCoreSignals();

        struct sigaction addrSignals;
        memset( &addrSignals, 0, sizeof( struct sigaction ) );
        addrSignals.sa_sigaction = abruptQuitWithAddrSignal;
        sigemptyset( &addrSignals.sa_mask );
        addrSignals.sa_flags = SA_SIGINFO;

        verify( sigaction(SIGSEGV, &addrSignals, 0) == 0 );
        verify( sigaction(SIGBUS, &addrSignals, 0) == 0 );
        verify( sigaction(SIGILL, &addrSignals, 0) == 0 );
        verify( sigaction(SIGFPE, &addrSignals, 0) == 0 );

        verify( signal(SIGABRT, abruptQuit) != SIG_ERR );
        verify( signal(SIGQUIT, abruptQuit) != SIG_ERR );
        verify( signal(SIGPIPE, SIG_IGN) != SIG_ERR );

        setupSIGTRAPforGDB();

        // asyncSignals is a global variable listing the signals that should be handled by the
        // interrupt thread, once it is started via startSignalProcessingThread().
        sigemptyset( &asyncSignals );
        sigaddset( &asyncSignals, SIGHUP );
        sigaddset( &asyncSignals, SIGINT );
        sigaddset( &asyncSignals, SIGTERM );
        sigaddset( &asyncSignals, SIGUSR1 );
        sigaddset( &asyncSignals, SIGXCPU );

        set_terminate( myterminate );
        set_new_handler( my_new_handler );
    }

    void startSignalProcessingThread() {
        verify( pthread_sigmask( SIG_SETMASK, &asyncSignals, 0 ) == 0 );
        boost::thread it( signalProcessingThread );
    }

#else   // WIN32
    void consoleTerminate( const char* controlCodeName ) {
        Client::initThread( "consoleTerminate" );
        log() << "got " << controlCodeName << ", will terminate after current cmd ends" << endl;
        exitCleanly( EXIT_KILL );
    }

    BOOL WINAPI CtrlHandler( DWORD fdwCtrlType ) {

        switch( fdwCtrlType ) {

        case CTRL_C_EVENT:
            rawOut( "Ctrl-C signal" );
            consoleTerminate( "CTRL_C_EVENT" );
            return TRUE ;

        case CTRL_CLOSE_EVENT:
            rawOut( "CTRL_CLOSE_EVENT signal" );
            consoleTerminate( "CTRL_CLOSE_EVENT" );
            return TRUE ;

        case CTRL_BREAK_EVENT:
            rawOut( "CTRL_BREAK_EVENT signal" );
            consoleTerminate( "CTRL_BREAK_EVENT" );
            return TRUE;

        case CTRL_LOGOFF_EVENT:
            // only sent to services, and only in pre-Vista Windows; FALSE means ignore
            return FALSE;

        case CTRL_SHUTDOWN_EVENT:
            rawOut( "CTRL_SHUTDOWN_EVENT signal" );
            consoleTerminate( "CTRL_SHUTDOWN_EVENT" );
            return TRUE;

        default:
            return FALSE;
        }
    }

    // called by mongoAbort()
    extern void (*reportEventToSystem)(const char *msg);
    void reportEventToSystemImpl(const char *msg) {
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

    void myPurecallHandler() {
        printStackTrace();
        mongoAbort("pure virtual");
    }

    void setupSignalHandlers() {
        reportEventToSystem = reportEventToSystemImpl;
        setWindowsUnhandledExceptionFilter();
        massert(10297,
                "Couldn't register Windows Ctrl-C handler",
                SetConsoleCtrlHandler(static_cast<PHANDLER_ROUTINE>(CtrlHandler), TRUE));
        _set_purecall_handler( myPurecallHandler );
    }

    void eventProcessingThread() {
        std::string eventName = getShutdownSignalName(ProcessId::getCurrent().asUInt32());

        HANDLE event = CreateEventA(NULL, TRUE, FALSE, eventName.c_str());
        if (event == NULL) {
            warning() << "eventProcessingThread CreateEvent failed: "
                << errnoWithDescription();
            return;
        }

        ON_BLOCK_EXIT(CloseHandle, event);

        int returnCode = WaitForSingleObject(event, INFINITE);
        if (returnCode != WAIT_OBJECT_0) {
            if (returnCode == WAIT_FAILED) {
                warning() << "eventProcessingThread WaitForSingleObject failed: "
                    << errnoWithDescription();
                return;
            }
            else {
                warning() << "eventProcessingThread WaitForSingleObject failed: "
                    << errnoWithDescription(returnCode);
                return;
            }
        }

        Client::initThread("eventTerminate");
        log() << "shutdown event signaled, will terminate after current cmd ends";
        exitCleanly(EXIT_CLEAN);
    }

    void startSignalProcessingThread() {
        if (Command::testCommandsEnabled) {
            boost::thread it(eventProcessingThread);
        }
    }

#endif  // if !defined(_WIN32)

} // namespace mongo
