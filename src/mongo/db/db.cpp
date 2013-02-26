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
*/

#include "mongo/pch.h"

#include <boost/thread/thread.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>

#include "mongo/base/initializer.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/d_globals.h"
#include "mongo/db/db.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/dur.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/module.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/repl.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/restapi.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/snapshots.h"
#include "mongo/db/ttl.h"
#include "mongo/s/d_writeback.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

#if !defined(_WIN32)
# include <sys/file.h>
#endif

namespace mongo {

    namespace dur {
        extern unsigned long long DataLimitPerJournalFile;
    }

    /* only off if --nohints */
    extern bool useHints;

    extern int diagLogging;
    extern unsigned lenForNewNsFiles;
    extern int lockFile;
    extern string repairpath;

    static void setupSignalHandlers();
    static void startInterruptThread();
    void startReplication();
    void exitCleanly( ExitCode code );

#ifdef _WIN32
    ntservice::NtServiceDefaultStrings defaultServiceStrings = {
        L"MongoDB",
        L"Mongo DB",
        L"Mongo DB Server"
    };
#endif

    CmdLine cmdLine;
    static bool scriptingEnabled = true;
    static bool noHttpInterface = false;
    bool shouldRepairDatabases = 0;
    static bool forceRepair = 0;
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

    void sysRuntimeInfo() {
        out() << "sysinfo:" << endl;
#if defined(_SC_PAGE_SIZE)
        out() << "  page size: " << (int) sysconf(_SC_PAGE_SIZE) << endl;
#endif
#if defined(_SC_PHYS_PAGES)
        out() << "  _SC_PHYS_PAGES: " << sysconf(_SC_PHYS_PAGES) << endl;
#endif
#if defined(_SC_AVPHYS_PAGES)
        out() << "  _SC_AVPHYS_PAGES: " << sysconf(_SC_AVPHYS_PAGES) << endl;
#endif
    }

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
            globalScriptEngine->threadDone();
        }

    };

    void logStartup() {
        BSONObjBuilder toLog;
        stringstream id;
        id << getHostNameCached() << "-" << jsTime();
        toLog.append( "_id", id.str() );
        toLog.append( "hostname", getHostNameCached() );

        toLog.appendTimeT( "startTime", time(0) );
        char buf[64];
        curTimeString( buf );
        toLog.append( "startTimeLocal", buf );

        toLog.append( "cmdLine", CmdLine::getParsedOpts() );
        toLog.append( "pid", getpid() );


        BSONObjBuilder buildinfo( toLog.subobjStart("buildinfo"));
        appendBuildInfo(buildinfo);
        buildinfo.doneFast();

        BSONObj o = toLog.obj();

        Lock::GlobalWrite lk;
        Client::GodScope gs;
        DBDirectClient c;
        const char* name = "local.startup_log";
        c.createCollection( name, 10 * 1024 * 1024, true );
        c.insert( name, o);
    }

    void listen(int port) {
        //testTheDb();
        MessageServer::Options options;
        options.port = port;
        options.ipList = cmdLine.bind_ip;

        MessageServer * server = createServer( options , new MyMessageHandler() );
        server->setAsTimeTracker();

        logStartup();
        startReplication();
        if ( !noHttpInterface )
            boost::thread web( boost::bind(&webServerThread, new RestAdminAccess() /* takes ownership */));

#if(TESTEXHAUST)
        boost::thread thr(testExhaust);
#endif
        server->run();
    }


    bool doDBUpgrade( const string& dbName , string errmsg , DataFileHeader * h ) {
        static DBDirectClient db;

        if ( h->version == 4 && h->versionMinor == 4 ) {
            verify( PDFILE_VERSION == 4 );
            verify( PDFILE_VERSION_MINOR == 5 );

            list<string> colls = db.getCollectionNames( dbName );
            for ( list<string>::iterator i=colls.begin(); i!=colls.end(); i++) {
                string c = *i;
                log() << "\t upgrading collection:" << c << endl;
                BSONObj out;
                bool ok = db.runCommand( dbName , BSON( "reIndex" << c.substr( dbName.size() + 1 ) ) , out );
                if ( ! ok ) {
                    errmsg = "reindex failed";
                    log() << "\t\t reindex failed: " << out << endl;
                    return false;
                }
            }

            h->versionMinor = 5;
            return true;
        }

        // do this in the general case
        return repairDatabase( dbName.c_str(), errmsg );
    }

    // ran at startup.
    static void repairDatabasesAndCheckVersion() {
        //        LastError * le = lastError.get( true );
        Client::GodScope gs;
        LOG(1) << "enter repairDatabases (to check pdfile version #)" << endl;

        Lock::GlobalWrite lk;
        vector< string > dbNames;
        getDatabaseNames( dbNames );
        for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
            string dbName = *i;
            LOG(1) << "\t" << dbName << endl;
            Client::Context ctx( dbName );
            MongoDataFile *p = cc().database()->getFile( 0 );
            DataFileHeader *h = p->getHeader();
            if ( !h->isCurrentVersion() || forceRepair ) {

                if( h->version <= 0 ) {
                    uasserted(14026,
                      str::stream() << "db " << dbName << " appears corrupt pdfile version: " << h->version
                                    << " info: " << h->versionMinor << ' ' << h->fileLength);
                }

                log() << "****" << endl;
                log() << "****" << endl;
                log() << "need to upgrade database " << dbName << " with pdfile version " << h->version << "." << h->versionMinor << ", "
                      << "new version: " << PDFILE_VERSION << "." << PDFILE_VERSION_MINOR << endl;
                if ( shouldRepairDatabases ) {
                    // QUESTION: Repair even if file format is higher version than code?
                    log() << "\t starting upgrade" << endl;
                    string errmsg;
                    verify( doDBUpgrade( dbName , errmsg , h ) );
                }
                else {
                    log() << "\t Not upgrading, exiting" << endl;
                    log() << "\t run --upgrade to upgrade dbs, then start again" << endl;
                    log() << "****" << endl;
                    dbexit( EXIT_NEED_UPGRADE );
                    shouldRepairDatabases = 1;
                    return;
                }
            }
            else {
                Database::closeDatabase( dbName.c_str(), dbpath );
            }
        }

        LOG(1) << "done repairDatabases" << endl;

        if ( shouldRepairDatabases ) {
            log() << "finished checking dbs" << endl;
            cc().shutdown();
            dbexit( EXIT_CLEAN );
        }
    }

    void clearTmpFiles() {
        boost::filesystem::path path( dbpath );
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
        if( !cmdLine.usingReplSets() ) {
            Client::GodScope gs;
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
            if( cmdLine.syncdelay == 0 )
                log() << "warning: --syncdelay 0 is not recommended and can have strange performance" << endl;
            else if( cmdLine.syncdelay == 1 )
                log() << "--syncdelay 1" << endl;
            else if( cmdLine.syncdelay != 60 )
                LOG(1) << "--syncdelay " << cmdLine.syncdelay << endl;
            int time_flushing = 0;
            while ( ! inShutdown() ) {
                _diaglog.flush();
                if ( cmdLine.syncdelay == 0 ) {
                    // in case at some point we add an option to change at runtime
                    sleepsecs(5);
                    continue;
                }

                sleepmillis( (long long) std::max(0.0, (cmdLine.syncdelay * 1000) - time_flushing) );

                if ( inShutdown() ) {
                    // occasional issue trying to flush during shutdown when sleep interrupted
                    break;
                }

                Date_t start = jsTime();
                int numFiles = MemoryMappedFile::flushAll( true );
                time_flushing = (int) (jsTime() - start);

                _flushed(time_flushing);

                if( logLevel >= 1 || time_flushing >= 10000 ) {
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

                if ( cmdLine.dur ) {
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
#endif // __linux__
    }

    void _initAndListen(int listenPort ) {

        Client::initThread("initandlisten");

        Logstream::get().addGlobalTee( new RamLog("global") );

        bool is32bit = sizeof(int*) == 4;

        {
#if !defined(_WIN32)
            pid_t pid = getpid();
#else
            DWORD pid=GetCurrentProcessId();
#endif
            Nullstream& l = log();
            l << "MongoDB starting : pid=" << pid << " port=" << cmdLine.port << " dbpath=" << dbpath;
            if( replSettings.master ) l << " master=" << replSettings.master;
            if( replSettings.slave )  l << " slave=" << (int) replSettings.slave;
            l << ( is32bit ? " 32" : " 64" ) << "-bit host=" << getHostNameCached() << endl;
        }
        DEV log() << "_DEBUG build (which is slower)" << endl;
        show_warnings();
        log() << mongodVersion() << endl;
        printGitVersion();
        printSysInfo();
        printAllocator();
        printCommandLineOpts();

        {
            stringstream ss;
            ss << endl;
            ss << "*********************************************************************" << endl;
            ss << " ERROR: dbpath (" << dbpath << ") does not exist." << endl;
            ss << " Create this directory or give existing directory in --dbpath." << endl;
            ss << " See http://dochub.mongodb.org/core/startingandstoppingmongo" << endl;
            ss << "*********************************************************************" << endl;
            uassert( 10296 ,  ss.str().c_str(), boost::filesystem::exists( dbpath ) );
        }
        {
            stringstream ss;
            ss << "repairpath (" << repairpath << ") does not exist";
            uassert( 12590 ,  ss.str().c_str(), boost::filesystem::exists( repairpath ) );
        }

        // TODO check non-journal subdirs if using directory-per-db
        checkReadAhead(dbpath);

        acquirePathLock(forceRepair);
        boost::filesystem::remove_all( dbpath + "/_tmp/" );

        FileAllocator::get()->start();

        MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( clearTmpFiles(), "clear tmp files" );

        dur::startup();

        if( cmdLine.durOptions & CmdLine::DurRecoverOnly )
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

        Module::initAll();

        if ( scriptingEnabled ) {
            ScriptEngine::setup();
            globalScriptEngine->setCheckInterruptCallback( jsInterruptCallback );
            globalScriptEngine->setGetCurrentOpIdCallback( jsGetCurrentOpIdCallback );
        }

        repairDatabasesAndCheckVersion();

        if ( shouldRepairDatabases )
            return;

        /* this is for security on certain platforms (nonce generation) */
        srand((unsigned) (curTimeMicros() ^ startupSrandTimer.micros()));

        snapshotThread.go();
        d.clientCursorMonitor.go();
        PeriodicTask::theRunner->go();
        if (missingRepl) {
            // a warning was logged earlier
        }
        else {
            startTTLBackgroundJob();
        }

#ifndef _WIN32
        CmdLine::launchOk();
#endif

        if( !noauth ) {
            // open admin db in case we need to use it later. TODO this is not the right way to
            // resolve this.
            Client::WriteContext c("admin", dbpath);
        }

        listen(listenPort);

        // listen() will return when exit code closes its socket.
        exitCleanly(EXIT_NET_ERROR);
    }

    void testPretouch();

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
        initAndListen( cmdLine.port );
    }
#endif

} // namespace mongo

using namespace mongo;

#include <boost/program_options.hpp>

namespace po = boost::program_options;

void show_help_text(po::options_description options) {
    show_warnings();
    cout << options << endl;
};

static int mongoDbMain(int argc, char* argv[], char** envp);

int main(int argc, char* argv[], char** envp) {
    int exitCode = mongoDbMain(argc, argv, envp);
    ::_exit(exitCode);
}

static void buildOptionsDescriptions(po::options_description *pVisible,
                                     po::options_description *pHidden,
                                     po::positional_options_description *pPositional) {

    po::options_description& visible_options = *pVisible;
    po::options_description& hidden_options = *pHidden;
    po::positional_options_description& positional_options = *pPositional;

    po::options_description general_options("General options");
#if defined(_WIN32)
    po::options_description windows_scm_options("Windows Service Control Manager options");
#endif
    po::options_description ms_options("Master/slave options (old; use replica sets instead)");
    po::options_description rs_options("Replica set options");
    po::options_description replication_options("Replication options");
    po::options_description sharding_options("Sharding options");
    po::options_description hidden_sharding_options("Sharding options");
    po::options_description ssl_options("SSL options");

    CmdLine::addGlobalOptions( general_options , hidden_options , ssl_options );

    StringBuilder dbpathBuilder;
    dbpathBuilder << "directory for datafiles - defaults to " << dbpath;

    general_options.add_options()
    ("auth", "run with security")
    ("cpu", "periodically show cpu and iowait utilization")
    ("dbpath", po::value<string>() , dbpathBuilder.str().c_str())
    ("diaglog", po::value<int>(), "0=off 1=W 2=R 3=both 7=W+some reads")
    ("directoryperdb", "each database will be stored in a separate directory")
    ("ipv6", "enable IPv6 support (disabled by default)")
    ("journal", "enable journaling")
    ("journalCommitInterval", po::value<unsigned>(), "how often to group/batch commit (ms)")
    ("journalOptions", po::value<int>(), "journal diagnostic options")
    ("jsonp","allow JSONP access via http (has security implications)")
    ("noauth", "run without security")
    ("nohttpinterface", "disable http interface")
        // SERVER-8536
        //   ("noIndexBuildRetry", "don't retry any index builds that were interrupted by shutdown")
    ("nojournal", "disable journaling (journaling is on by default for 64 bit)")
    ("noprealloc", "disable data file preallocation - will often hurt performance")
    ("noscripting", "disable scripting engine")
    ("notablescan", "do not allow table scans")
    ("nssize", po::value<int>()->default_value(16), ".ns file size (in MB) for new databases")
    ("profile",po::value<int>(), "0=off 1=slow, 2=all")
    ("quota", "limits each database to a certain number of files (8 default)")
    ("quotaFiles", po::value<int>(), "number of files allowed per db, requires --quota")
    ("repair", "run repair on all dbs")
    ("repairpath", po::value<string>() , "root directory for repair files - defaults to dbpath" )
    ("rest","turn on simple rest api")
#if defined(__linux__)
    ("shutdown", "kill a running server (for init scripts)")
#endif
    ("slowms",po::value<int>(&cmdLine.slowMS)->default_value(100), "value of slow for profile and console log" )
    ("smallfiles", "use a smaller default file size")
    ("syncdelay",po::value<double>(&cmdLine.syncdelay)->default_value(60), "seconds between disk syncs (0=never, but not recommended)")
    ("sysinfo", "print some diagnostic system information")
    ("upgrade", "upgrade db if needed")
    ;

#if defined(_WIN32)
    CmdLine::addWindowsOptions( windows_scm_options, hidden_options );
#endif

    replication_options.add_options()
    ("oplogSize", po::value<int>(), "size to use (in MB) for replication op log. default is 5% of disk space (i.e. large is good)")
    ;

    ms_options.add_options()
    ("master", "master mode")
    ("slave", "slave mode")
    ("source", po::value<string>(), "when slave: specify master as <server:port>")
    ("only", po::value<string>(), "when slave: specify a single database to replicate")
    ("slavedelay", po::value<int>(), "specify delay (in seconds) to be used when applying master ops to slave")
    ("autoresync", "automatically resync if slave data is stale")
    ;

    rs_options.add_options()
    ("replSet", po::value<string>(), "arg is <setname>[/<optionalseedhostlist>]")
    ("replIndexPrefetch", po::value<string>(), "specify index prefetching behavior (if secondary) [none|_id_only|all]")
    ;

    sharding_options.add_options()
    ("configsvr", "declare this is a config db of a cluster; default port 27019; default dir /data/configdb")
    ("shardsvr", "declare this is a shard db of a cluster; default port 27018")
    ;

    hidden_sharding_options.add_options()
    ("noMoveParanoia" , "turn off paranoid saving of data for the moveChunk command; default" )
    ("moveParanoia" , "turn on paranoid saving of data during the moveChunk command (used for internal system diagnostics)" )
    ;
    hidden_options.add(hidden_sharding_options);

    hidden_options.add_options()
    ("fastsync", "indicate that this instance is starting from a dbpath snapshot of the repl peer")
    ("pretouch", po::value<int>(), "n pretouch threads for applying replicationed operations") // experimental
    ("command", po::value< vector<string> >(), "command")
    ("cacheSize", po::value<long>(), "cache size (in MB) for rec store")
    ("nodur", "disable journaling")
    // things we don't want people to use
    ("nohints", "ignore query hints")
    ("nopreallocj", "don't preallocate journal files")
    ("dur", "enable journaling") // old name for --journal
    ("durOptions", po::value<int>(), "durability diagnostic options") // deprecated name
    // deprecated pairing command line options
    ("pairwith", "DEPRECATED")
    ("arbiter", "DEPRECATED")
    ("opIdMem", "DEPRECATED")
    ;

    positional_options.add("command", 3);
    visible_options.add(general_options);
#if defined(_WIN32)
    visible_options.add(windows_scm_options);
#endif
    visible_options.add(replication_options);
    visible_options.add(ms_options);
    visible_options.add(rs_options);
    visible_options.add(sharding_options);
#ifdef MONGO_SSL
    visible_options.add(ssl_options);
#endif
    Module::addOptions( visible_options );
}

static void processCommandLineOptions(const std::vector<std::string>& argv) {
    po::options_description visible_options("Allowed options");
    po::options_description hidden_options("Hidden options");
    po::positional_options_description positional_options;
    buildOptionsDescriptions(&visible_options, &hidden_options, &positional_options);

    {
        po::variables_map params;

        if (!CmdLine::store(argv,
                            visible_options,
                            hidden_options,
                            positional_options,
                            params)) {
            ::_exit(EXIT_FAILURE);
        }

        if (params.count("help")) {
            show_help_text(visible_options);
            ::_exit(EXIT_SUCCESS);
        }
        if (params.count("version")) {
            cout << mongodVersion() << endl;
            printGitVersion();
            ::_exit(EXIT_SUCCESS);
        }
        if (params.count("sysinfo")) {
            sysRuntimeInfo();
            ::_exit(EXIT_SUCCESS);
        }

        if ( params.count( "dbpath" ) ) {
            dbpath = params["dbpath"].as<string>();
            if ( params.count( "fork" ) && dbpath[0] != '/' ) {
                // we need to change dbpath if we fork since we change
                // cwd to "/"
                // fork only exists on *nix
                // so '/' is safe
                dbpath = cmdLine.cwd + "/" + dbpath;
            }
        }
#ifdef _WIN32
        if (dbpath.size() > 1 && dbpath[dbpath.size()-1] == '/') {
            // size() check is for the unlikely possibility of --dbpath "/"
            dbpath = dbpath.erase(dbpath.size()-1);
        }
#endif

        if ( params.count("directoryperdb")) {
            directoryperdb = true;
        }
        if (params.count("cpu")) {
            cmdLine.cpu = true;
        }
        if (params.count("noauth")) {
            noauth = true;
        }
        if (params.count("auth")) {
            noauth = false;
        }
        if (params.count("quota")) {
            cmdLine.quota = true;
        }
        if (params.count("quotaFiles")) {
            cmdLine.quota = true;
            cmdLine.quotaFiles = params["quotaFiles"].as<int>() - 1;
        }
        bool journalExplicit = false;
        if( params.count("nodur") || params.count( "nojournal" ) ) {
            journalExplicit = true;
            cmdLine.dur = false;
        }
        if( params.count("dur") || params.count( "journal" ) ) {
            if (journalExplicit) {
                log() << "Can't specify both --journal and --nojournal options." << endl;
                ::_exit(EXIT_BADOPTIONS);
            }
            journalExplicit = true;
            cmdLine.dur = true;
        }
        if (params.count("durOptions")) {
            cmdLine.durOptions = params["durOptions"].as<int>();
        }
        if( params.count("journalCommitInterval") ) {
            // don't check if dur is false here as many will just use the default, and will default to off on win32.
            // ie no point making life a little more complex by giving an error on a dev environment.
            cmdLine.journalCommitInterval = params["journalCommitInterval"].as<unsigned>();
            if( cmdLine.journalCommitInterval <= 1 || cmdLine.journalCommitInterval > 300 ) {
                out() << "--journalCommitInterval out of allowed range (0-300ms)" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
        }
        if (params.count("journalOptions")) {
            cmdLine.durOptions = params["journalOptions"].as<int>();
        }
        if (params.count("repairpath")) {
            repairpath = params["repairpath"].as<string>();
            if (!repairpath.size()) {
                out() << "repairpath is empty" << endl;
                dbexit( EXIT_BADOPTIONS );
            }

            if (cmdLine.dur && !str::startsWith(repairpath, dbpath)) {
                out() << "You must use a --repairpath that is a subdirectory of --dbpath when using journaling" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
        }
        if (params.count("nohints")) {
            useHints = false;
        }
        if (params.count("nopreallocj")) {
            cmdLine.preallocj = false;
        }
        if (params.count("nohttpinterface")) {
            noHttpInterface = true;
        }
        if (params.count("rest")) {
            cmdLine.rest = true;
        }
        if (params.count("jsonp")) {
            cmdLine.jsonp = true;
        }
        if (params.count("noscripting")) {
            scriptingEnabled = false;
        }
        if (params.count("noprealloc")) {
            cmdLine.prealloc = false;
            cout << "note: noprealloc may hurt performance in many applications" << endl;
        }
        if (params.count("smallfiles")) {
            cmdLine.smallfiles = true;
            verify( dur::DataLimitPerJournalFile >= 128 * 1024 * 1024 );
            dur::DataLimitPerJournalFile = 128 * 1024 * 1024;
        }
        if (params.count("diaglog")) {
            int x = params["diaglog"].as<int>();
            if ( x < 0 || x > 7 ) {
                out() << "can't interpret --diaglog setting" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            _diaglog.setLevel(x);
        }
        if (params.count("repair")) {
            if (journalExplicit && cmdLine.dur) {
                log() << "Can't specify both --journal and --repair options." << endl;
                ::_exit(EXIT_BADOPTIONS);
            }

            Record::MemoryTrackingEnabled = false;
            shouldRepairDatabases = 1;
            forceRepair = 1;
            cmdLine.dur = false;
        }
        if (params.count("upgrade")) {
            Record::MemoryTrackingEnabled = false;
            shouldRepairDatabases = 1;
        }
        if (params.count("notablescan")) {
            cmdLine.noTableScan = true;
        }
        if (params.count("master")) {
            replSettings.master = true;
        }
        if (params.count("slave")) {
            replSettings.slave = SimpleSlave;
        }
        if (params.count("slavedelay")) {
            replSettings.slavedelay = params["slavedelay"].as<int>();
        }
        if (params.count("fastsync")) {
            replSettings.fastsync = true;
        }
        if (params.count("autoresync")) {
            replSettings.autoresync = true;
            if( params.count("replSet") ) {
                out() << "--autoresync is not used with --replSet" << endl;
                out() << "see http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
        }
        if (params.count("source")) {
            /* specifies what the source in local.sources should be */
            cmdLine.source = params["source"].as<string>().c_str();
        }
        if( params.count("pretouch") ) {
            cmdLine.pretouch = params["pretouch"].as<int>();
        }
        if (params.count("replSet")) {
            if (params.count("slavedelay")) {
                out() << "--slavedelay cannot be used with --replSet" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            else if (params.count("only")) {
                out() << "--only cannot be used with --replSet" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            /* seed list of hosts for the repl set */
            cmdLine._replSet = params["replSet"].as<string>().c_str();
        }
        if (params.count("replIndexPrefetch")) {
            cmdLine.rsIndexPrefetch = params["replIndexPrefetch"].as<std::string>();
        }
        if (params.count("noIndexBuildRetry")) {
            cmdLine.indexBuildRetry = false;
        }
        if (params.count("only")) {
            cmdLine.only = params["only"].as<string>().c_str();
        }
        if( params.count("nssize") ) {
            int x = params["nssize"].as<int>();
            if (x <= 0 || x > (0x7fffffff/1024/1024)) {
                out() << "bad --nssize arg" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            lenForNewNsFiles = x * 1024 * 1024;
            verify(lenForNewNsFiles > 0);
        }
        if (params.count("oplogSize")) {
            long long x = params["oplogSize"].as<int>();
            if (x <= 0) {
                out() << "bad --oplogSize arg" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            // note a small size such as x==1 is ok for an arbiter.
            if( x > 1000 && sizeof(void*) == 4 ) {
                out() << "--oplogSize of " << x << "MB is too big for 32 bit version. Use 64 bit build instead." << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            cmdLine.oplogSize = x * 1024 * 1024;
            verify(cmdLine.oplogSize > 0);
        }
        if (params.count("cacheSize")) {
            long x = params["cacheSize"].as<long>();
            if (x <= 0) {
                out() << "bad --cacheSize arg" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            log() << "--cacheSize option not currently supported" << endl;
        }
        if (params.count("port") == 0 ) {
            if( params.count("configsvr") ) {
                cmdLine.port = CmdLine::ConfigServerPort;
            }
            if( params.count("shardsvr") ) {
                if( params.count("configsvr") ) {
                    log() << "can't do --shardsvr and --configsvr at the same time" << endl;
                    dbexit( EXIT_BADOPTIONS );
                }
                cmdLine.port = CmdLine::ShardServerPort;
            }
        }
        else {
            if ( cmdLine.port <= 0 || cmdLine.port > 65535 ) {
                out() << "bad --port number" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
        }
        if ( params.count("configsvr" ) ) {
            cmdLine.configsvr = true;
            cmdLine.smallfiles = true; // config server implies small files
            dur::DataLimitPerJournalFile = 128 * 1024 * 1024;
            if (cmdLine.usingReplSets() || replSettings.master || replSettings.slave) {
                log() << "replication should not be enabled on a config server" << endl;
                ::_exit(-1);
            }
            if ( params.count( "nodur" ) == 0 && params.count( "nojournal" ) == 0 )
                cmdLine.dur = true;
            if ( params.count( "dbpath" ) == 0 )
                dbpath = "/data/configdb";
            replSettings.master = true;
            if ( params.count( "oplogSize" ) == 0 )
                cmdLine.oplogSize = 5 * 1024 * 1024;
        }
        if ( params.count( "profile" ) ) {
            cmdLine.defaultProfile = params["profile"].as<int>();
        }
        if (params.count("ipv6")) {
            enableIPv6();
        }

        if (params.count("noMoveParanoia") > 0 && params.count("moveParanoia") > 0) {
            out() << "The moveParanoia and noMoveParanoia flags cannot both be set; please use only one of them." << endl;
            ::_exit( EXIT_BADOPTIONS );
        }

        if (params.count("noMoveParanoia"))
            cmdLine.moveParanoia = false;

        if (params.count("moveParanoia"))
            cmdLine.moveParanoia = true;

        if (params.count("pairwith") || params.count("arbiter") || params.count("opIdMem")) {
            out() << "****" << endl;
            out() << "Replica Pairs have been deprecated. Invalid options: --pairwith, --arbiter, and/or --opIdMem" << endl;
            out() << "<http://dochub.mongodb.org/core/replicapairs>" << endl;
            out() << "****" << endl;
            dbexit( EXIT_BADOPTIONS );
        }

        // needs to be after things like --configsvr parsing, thus here.
        if( repairpath.empty() )
            repairpath = dbpath;

        // The "command" option is deprecated.  For backward compatibility, still support the "run"
        // and "dbppath" command.  The "run" command is the same as just running mongod, so just
        // falls through.
        if (params.count("command")) {
            vector<string> command = params["command"].as< vector<string> >();

            if (command[0].compare("dbpath") == 0) {
                cout << dbpath << endl;
                ::_exit(EXIT_SUCCESS);
            }

            if (command[0].compare("run") != 0) {
                cout << "Invalid command: " << command[0] << endl;
                cout << visible_options << endl;
                ::_exit(EXIT_FAILURE);
            }

            if (command.size() > 1) {
                cout << "Too many parameters to 'run' command" << endl;
                cout << visible_options << endl;
                ::_exit(EXIT_FAILURE);
            }
        }

        if( cmdLine.pretouch )
            log() << "--pretouch " << cmdLine.pretouch << endl;

        if (sizeof(void*) == 4 && !journalExplicit){
            // trying to make this stand out more like startup warnings
            log() << endl;
            warning() << "32-bit servers don't have journaling enabled by default. Please use --journal if you want durability." << endl;
            log() << endl;
        }

        Module::configAll(params);

#ifdef _WIN32
        ntservice::configureService(initService,
                                    params,
                                    defaultServiceStrings,
                                    std::vector<std::string>(),
                                    argv);
#endif  // _WIN32

#ifdef __linux__
        if (params.count("shutdown")){
            bool failed = false;

            string name = ( boost::filesystem::path( dbpath ) / "mongod.lock" ).string();
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
                cerr << "There doesn't seem to be a server running with dbpath: " << dbpath << endl;
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
}

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


    processCommandLineOptions(std::vector<std::string>(argv, argv + argc));
    mongo::runGlobalInitializersOrDie(argc, argv, envp);
    CmdLine::censor(argc, argv);

    if (!initializeServerGlobalState())
        ::_exit(EXIT_FAILURE);

    // Per SERVER-7434, startInterruptThread() must run after any forks
    // (initializeServerGlobalState()) and before creation of any other threads.
    startInterruptThread();

    dataFileSync.go();

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // exits directly and so never reaches here either.
    }
#endif

    StartupTest::runTests();
    initAndListen(cmdLine.port);
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
    // The above signals will be processed by this thread only, in order to
    // ensure the db and log mutexes aren't held.
    void interruptThread() {
        int actualSignal;
        sigwait( &asyncSignals, &actualSignal );
        log() << "got signal " << actualSignal << " (" << strsignal( actualSignal )
              << "), will terminate after current cmd ends" << endl;
        Client::initThread( "interruptThread" );
        exitCleanly( EXIT_CLEAN );
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
        // interrupt thread, once it is started via startInterruptThread().
        sigemptyset( &asyncSignals );
        sigaddset( &asyncSignals, SIGHUP );
        sigaddset( &asyncSignals, SIGINT );
        sigaddset( &asyncSignals, SIGTERM );

        set_terminate( myterminate );
        set_new_handler( my_new_handler );
    }

    void startInterruptThread() {
        verify( pthread_sigmask( SIG_SETMASK, &asyncSignals, 0 ) == 0 );
        boost::thread it( interruptThread );
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

    void startInterruptThread() {}

#endif  // if !defined(_WIN32)

} // namespace mongo
