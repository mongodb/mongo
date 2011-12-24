// server.cpp

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

#include "pch.h"
#include "../util/net/message.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../util/net/message_server.h"
#include "../util/stringutils.h"
#include "../util/version.h"
#include "../util/ramlog.h"
#include "../util/signal_handlers.h"
#include "../util/admin_access.h"
#include "../util/concurrency/task.h"
#include "../db/dbwebserver.h"
#include "../scripting/engine.h"

#include "server.h"
#include "request.h"
#include "client.h"
#include "config.h"
#include "chunk.h"
#include "balance.h"
#include "grid.h"
#include "cursors.h"
#include "shard_version.h"

namespace mongo {

    CmdLine cmdLine;
    Database *database = 0;
    string mongosCommand;
    bool dbexitCalled = false;
    static bool scriptingEnabled = true;

    bool inShutdown() {
        return dbexitCalled;
    }

    string getDbContext() {
        return "?";
    }

    bool haveLocalShardingInfo( const string& ns ) {
        assert( 0 );
        return false;
    }

    void usage( char * argv[] ) {
        out() << argv[0] << " usage:\n\n";
        out() << " -v+  verbose 1: general 2: more 3: per request 4: more\n";
        out() << " --port <portno>\n";
        out() << " --configdb <configdbname>,[<configdbname>,<configdbname>]\n";
        out() << endl;
    }

    void ShardingConnectionHook::onHandedOut( DBClientBase * conn ) {
        ClientInfo::get()->addShard( conn->getServerAddress() );
    }

    class ShardedMessageHandler : public MessageHandler {
    public:
        virtual ~ShardedMessageHandler() {}

        virtual void connected( AbstractMessagingPort* p ) {
            ClientInfo *c = ClientInfo::get();
            massert(15849, "client info not defined", c);
            c->getAuthenticationInfo()->isLocalHost = p->remote().isLocalHost();
        }

        virtual void process( Message& m , AbstractMessagingPort* p , LastError * le) {
            assert( p );
            Request r( m , p );

            assert( le );            
            lastError.startRequest( m , le );

            try {
                r.init();
                r.process();
            }
            catch ( AssertionException & e ) {
                log( e.isUserAssertion() ? 1 : 0 ) << "AssertionException while processing op type : " << m.operation() << " to : " << r.getns() << causedBy(e) << endl;

                le->raiseError( e.getCode() , e.what() );

                m.header()->id = r.id();

                if ( r.expectResponse() ) {
                    BSONObj err = BSON( "$err" << e.what() << "code" << e.getCode() );
                    replyToQuery( ResultFlag_ErrSet, p , m , err );
                }
            }
            catch ( DBException& e ) {
                log() << "DBException in process: " << e.what() << endl;

                le->raiseError( e.getCode() , e.what() );

                m.header()->id = r.id();

                if ( r.expectResponse() ) {
                    BSONObj err = BSON( "$err" << e.what() << "code" << e.getCode() );
                    replyToQuery( ResultFlag_ErrSet, p , m , err );
                }
            }
        }

        virtual void disconnected( AbstractMessagingPort* p ) {
            // all things are thread local
        }
    };

    void sighandler(int sig) {
        dbexit(EXIT_CLEAN, (string("received signal ") + BSONObjBuilder::numStr(sig)).c_str());
    }

    // this gets called when new fails to allocate memory
    void my_new_handler() {
        rawOut( "out of memory, printing stack and exiting:" );
        printStackTrace();
        ::exit(EXIT_ABRUPT);
    }

    void setupSignals( bool inFork ) {
        signal(SIGTERM, sighandler);
        signal(SIGINT, sighandler);

#if defined(SIGQUIT)
        signal( SIGQUIT , printStackAndExit );
#endif
        signal( SIGSEGV , printStackAndExit );
        signal( SIGABRT , printStackAndExit );
        signal( SIGFPE , printStackAndExit );
#if defined(SIGBUS)
        signal( SIGBUS , printStackAndExit );
#endif

        set_new_handler( my_new_handler );
    }

    void init() {
        serverID.init();
        setupSIGTRAPforGDB();
        setupCoreSignals();
        setupSignals( false );
        Logstream::get().addGlobalTee( new RamLog("global") );
    }

    void start( const MessageServer::Options& opts ) {
        setThreadName( "mongosMain" );

        balancer.go();
        cursorCache.startTimeoutThread();
        PeriodicTask::theRunner->go();

        ShardedMessageHandler handler;
        MessageServer * server = createServer( opts , &handler );
        server->setAsTimeTracker();
        server->run();
    }

    DBClientBase *createDirectClient() {
        uassert( 10197 ,  "createDirectClient not implemented for sharding yet" , 0 );
        return 0;
    }

    void printShardingVersionInfo(bool out) {
        if (out) {
          cout << mongosCommand << " " << mongodVersion() << " starting (--help for usage)" << endl;
          cout << "git version: " << gitVersion() << endl;
          cout <<  "build sys info: " << sysInfo() << endl;
        } else {
          log() << mongosCommand << " " << mongodVersion() << " starting (--help for usage)" << endl;
          printGitVersion();
          printSysInfo();
        }
    }

    void cloudCmdLineParamIs(string cmd);

} // namespace mongo

using namespace mongo;

#include <boost/program_options.hpp>

namespace po = boost::program_options;

int _main(int argc, char* argv[]) {
    static StaticObserver staticObserver;
    mongosCommand = argv[0];

    po::options_description options("General options");
    po::options_description sharding_options("Sharding options");
    po::options_description hidden("Hidden options");
    po::positional_options_description positional;

    CmdLine::addGlobalOptions( options , hidden );

    sharding_options.add_options()
    ( "configdb" , po::value<string>() , "1 or 3 comma separated config servers" )
    ( "test" , "just run unit tests" )
    ( "upgrade" , "upgrade meta data version" )
    ( "chunkSize" , po::value<int>(), "maximum amount of data per chunk" )
    ( "ipv6", "enable IPv6 support (disabled by default)" )
    ( "jsonp","allow JSONP access via http (has security implications)" )
    ("noscripting", "disable scripting engine")
    ;

    options.add(sharding_options);
    // parse options
    po::variables_map params;
    if ( ! CmdLine::store( argc , argv , options , hidden , positional , params ) )
        return 0;

    // The default value may vary depending on compile options, but for mongos
    // we want durability to be disabled.
    cmdLine.dur = false;

    if ( params.count( "help" ) ) {
        cout << options << endl;
        return 0;
    }

    if ( params.count( "version" ) ) {
        printShardingVersionInfo(true);
        return 0;
    }

    if ( params.count( "chunkSize" ) ) {
        int csize = params["chunkSize"].as<int>();
    
        // validate chunksize before proceeding
        if ( csize == 0 ) {
            out() << "error: need a non-zero chunksize" << endl;
            return 11;
        }

        Chunk::MaxChunkSize = csize * 1024 * 1024;
    }

    if ( params.count( "ipv6" ) ) {
        enableIPv6();
    }

    if ( params.count( "jsonp" ) ) {
        cmdLine.jsonp = true;
    }

    if ( params.count( "test" ) ) {
        logLevel = 5;
        UnitTest::runTests();
        cout << "tests passed" << endl;
        return 0;
    }

    if (params.count("noscripting")) {
        scriptingEnabled = false;
    }

    if ( ! params.count( "configdb" ) ) {
        out() << "error: no args for --configdb" << endl;
        return 4;
    }

    if( params.count("cloud") ) {
        string s = params["cloud"].as<string>();
        cloudCmdLineParamIs(s);
    }

    vector<string> configdbs;
    splitStringDelim( params["configdb"].as<string>() , &configdbs , ',' );
    if ( configdbs.size() != 1 && configdbs.size() != 3 ) {
        out() << "need either 1 or 3 configdbs" << endl;
        return 5;
    }

    // we either have a setting where all processes are in localhost or none are
    for ( vector<string>::const_iterator it = configdbs.begin() ; it != configdbs.end() ; ++it ) {
        try {

            HostAndPort configAddr( *it );  // will throw if address format is invalid

            if ( it == configdbs.begin() ) {
                grid.setAllowLocalHost( configAddr.isLocalHost() );
            }

            if ( configAddr.isLocalHost() != grid.allowLocalHost() ) {
                out() << "cannot mix localhost and ip addresses in configdbs" << endl;
                return 10;
            }

        }
        catch ( DBException& e) {
            out() << "configdb: " << e.what() << endl;
            return 9;
        }
    }
    
    // set some global state

    pool.addHook( new ShardingConnectionHook( false ) );
    pool.setName( "mongos connectionpool" );

    shardConnectionPool.addHook( new ShardingConnectionHook( true ) );
    shardConnectionPool.setName( "mongos shardconnection connectionpool" );

    
    DBClientConnection::setLazyKillCursor( false );

    ReplicaSetMonitor::setConfigChangeHook( boost::bind( &ConfigServer::replicaSetChange , &configServer , _1 ) );
    
    if ( argc <= 1 ) {
        usage( argv );
        return 3;
    }

    bool ok = cmdLine.port != 0 && configdbs.size();

    if ( !ok ) {
        usage( argv );
        return 1;
    }

    printShardingVersionInfo(false);

    if ( ! configServer.init( configdbs ) ) {
        cout << "couldn't resolve config db address" << endl;
        return 7;
    }

    if ( ! configServer.ok( true ) ) {
        cout << "configServer connection startup check failed" << endl;
        return 8;
    }

    {
        class CheckConfigServers : public task::Task {
            virtual string name() const { return "CheckConfigServers"; }
            virtual void doWork() { configServer.ok(true); }
        };
        static CheckConfigServers checkConfigServers;

        task::repeat(&checkConfigServers, 60*1000);
    }

    int configError = configServer.checkConfigVersion( params.count( "upgrade" ) );
    if ( configError ) {
        if ( configError > 0 ) {
            cout << "upgrade success!" << endl;
        }
        else {
            cout << "config server error: " << configError << endl;
        }
        return configError;
    }
    configServer.reloadSettings();
    
    init();

#ifndef _WIN32
    CmdLine::launchOk();
#endif

    boost::thread web( boost::bind(&webServerThread, new NoAdminAccess() /* takes ownership */) );

    if ( scriptingEnabled ) {
        ScriptEngine::setup();
//        globalScriptEngine->setCheckInterruptCallback( jsInterruptCallback );
//        globalScriptEngine->setGetInterruptSpecCallback( jsGetInterruptSpecCallback );
    }

    MessageServer::Options opts;
    opts.port = cmdLine.port;
    opts.ipList = cmdLine.bind_ip;
    start(opts);

    dbexit( EXIT_CLEAN );
    return 0;
}
int main(int argc, char* argv[]) {
    try {
        doPreServerStartupInits();
        return _main(argc, argv);
    }
    catch(DBException& e) { 
        cout << "uncaught exception in mongos main:" << endl;
        cout << e.toString() << endl;
    }
    catch(std::exception& e) { 
        cout << "uncaught exception in mongos main:" << endl;
        cout << e.what() << endl;
    }
    catch(...) { 
        cout << "uncaught exception in mongos main" << endl;
    }
    return 20;
}

#undef exit

void mongo::exitCleanly( ExitCode code ) {
    // TODO: do we need to add anything?
    mongo::dbexit( code );
}

void mongo::dbexit( ExitCode rc, const char *why, bool tryToGetLock ) {
    dbexitCalled = true;
    log() << "dbexit: " << why
          << " rc:" << rc
          << " " << ( why ? why : "" )
          << endl;
    ::exit(rc);
}
