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
#include "../util/message.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../util/message_server.h"
#include "../util/stringutils.h"
#include "../util/version.h"
#include "../util/signal_handlers.h"
#include "../db/dbwebserver.h"

#include "server.h"
#include "request.h"
#include "config.h"
#include "chunk.h"
#include "balance.h"
#include "grid.h"
#include "cursors.h"

namespace mongo {
    
    CmdLine cmdLine;    
    Database *database = 0;
    string mongosCommand;
    bool dbexitCalled = false;

    bool inShutdown(){
        return dbexitCalled;
    }
    
    string getDbContext() {
        return "?";
    }

    bool haveLocalShardingInfo( const string& ns ){
        assert( 0 );
        return false;
    }
    
    void usage( char * argv[] ){
        out() << argv[0] << " usage:\n\n";
        out() << " -v+  verbose 1: general 2: more 3: per request 4: more\n";
        out() << " --port <portno>\n";
        out() << " --configdb <configdbname>,[<configdbname>,<configdbname>]\n";
        out() << endl;
    }

    class ShardingConnectionHook : public DBConnectionHook {
    public:

        virtual void onHandedOut( DBClientBase * conn ){
            ClientInfo::get()->addShard( conn->getServerAddress() );
        }
    } shardingConnectionHook;
    
    class ShardedMessageHandler : public MessageHandler {
    public:
        virtual ~ShardedMessageHandler(){}

        virtual void process( Message& m , AbstractMessagingPort* p ){
            assert( p );
            Request r( m , p );

            LastError * le = lastError.startRequest( m , r.getClientId() );
            assert( le );
            
            if ( logLevel > 5 ){
                log(5) << "client id: " << hex << r.getClientId() << "\t" << r.getns() << "\t" << dec << r.op() << endl;
            }
            try {
                r.init();
                setClientId( r.getClientId() );
                r.process();
            }
            catch ( DBException& e ){
                log() << "DBException in process: " << e.what() << endl;
                
                le->raiseError( e.getCode() , e.what() );
                
                m.header()->id = r.id();
                
                if ( r.expectResponse() ){
                    BSONObj err = BSON( "$err" << e.what() << "code" << e.getCode() );
                    replyToQuery( ResultFlag_ErrSet, p , m , err );
                }
            }
        }

        virtual void disconnected( AbstractMessagingPort* p ){
            ClientInfo::disconnect( p->getClientId() );
            lastError.disconnect( p->getClientId() );
        }
    };

    void sighandler(int sig){
        dbexit(EXIT_CLEAN, (string("received signal ") + BSONObjBuilder::numStr(sig)).c_str());
    }
    
    void setupSignals(){
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
    }

    void init(){
        serverID.init();
        setupSIGTRAPforGDB();
        setupCoreSignals();
        setupSignals();
    }

    void start( const MessageServer::Options& opts ){
        balancer.go();
        cursorCache.startTimeoutThread();

        log() << "waiting for connections on port " << cmdLine.port << endl;
        //DbGridListener l(port);
        //l.listen();
        ShardedMessageHandler handler;
        MessageServer * server = createServer( opts , &handler );
        server->setAsTimeTracker();
        server->run();
    }

    DBClientBase *createDirectClient(){
        uassert( 10197 ,  "createDirectClient not implemented for sharding yet" , 0 );
        return 0;
    }

    void printShardingVersionInfo(){
        log() << mongosCommand << " " << mongodVersion() << " starting (--help for usage)" << endl;
        printGitVersion();
        printSysInfo();
    }

} // namespace mongo

using namespace mongo;

#include <boost/program_options.hpp>

namespace po = boost::program_options;

int main(int argc, char* argv[], char *envp[] ) {
    static StaticObserver staticObserver;
    mongosCommand = argv[0];

    po::options_description options("Sharding options");
    po::options_description hidden("Hidden options");
    po::positional_options_description positional;
    
    CmdLine::addGlobalOptions( options , hidden );
    
    options.add_options()
        ( "configdb" , po::value<string>() , "1 or 3 comma separated config servers" )
        ( "test" , "just run unit tests" )
        ( "upgrade" , "upgrade meta data version" )
        ( "chunkSize" , po::value<int>(), "maximum amount of data per chunk" )
        ( "ipv6", "enable IPv6 support (disabled by default)" )
        ;
    

    // parse options
    po::variables_map params;
    if ( ! CmdLine::store( argc , argv , options , hidden , positional , params ) )
        return 0;
    
    if ( params.count( "help" ) ){
        cout << options << endl;
        return 0;
    }

    if ( params.count( "version" ) ){
        printShardingVersionInfo();
        return 0;
    }

    if ( params.count( "chunkSize" ) ){
        Chunk::MaxChunkSize = params["chunkSize"].as<int>() * 1024 * 1024;
    }

    if ( params.count( "ipv6" ) ){
        enableIPv6();
    }

    if ( params.count( "test" ) ){
        logLevel = 5;
        UnitTest::runTests();
        cout << "tests passed" << endl;
        return 0;
    }
    
    if ( ! params.count( "configdb" ) ){
        out() << "error: no args for --configdb" << endl;
        return 4;
    }

    vector<string> configdbs;
    splitStringDelim( params["configdb"].as<string>() , &configdbs , ',' );
    if ( configdbs.size() != 1 && configdbs.size() != 3 ){
        out() << "need either 1 or 3 configdbs" << endl;
        return 5;
    }

    // we either have a seeting were all process are in localhost or none is
    for ( vector<string>::const_iterator it = configdbs.begin() ; it != configdbs.end() ; ++it ){
        try {

            HostAndPort configAddr( *it );  // will throw if address format is invalid

            if ( it == configdbs.begin() ){
                grid.setAllowLocalHost( configAddr.isLocalHost() );
            }

            if ( configAddr.isLocalHost() != grid.allowLocalHost() ){
                out() << "cannot mix localhost and ip addresses in configdbs" << endl;
                return 10;
            }

        } 
        catch ( DBException& e) {
            out() << "configdb: " << e.what() << endl;
            return 9;
        }
    }
    
    pool.addHook( &shardingConnectionHook );

    if ( argc <= 1 ) {
        usage( argv );
        return 3;
    }

    bool ok = cmdLine.port != 0 && configdbs.size();

    if ( !ok ) {
        usage( argv );
        return 1;
    }
    
    printShardingVersionInfo();
    
    if ( ! configServer.init( configdbs ) ){
        cout << "couldn't resolve config db address" << endl;
        return 7;
    }
    
    if ( ! configServer.ok( true ) ){
        cout << "configServer startup check failed" << endl;
        return 8;
    }
    
    int configError = configServer.checkConfigVersion( params.count( "upgrade" ) );
    if ( configError ){
        if ( configError > 0 ){
            cout << "upgrade success!" << endl;
        }
        else {
            cout << "config server error: " << configError << endl;
        }
        return configError;
    }
    configServer.reloadSettings();

    init();

    boost::thread web( webServerThread );
    
    MessageServer::Options opts;
    opts.port = cmdLine.port;
    opts.ipList = cmdLine.bind_ip;
    start(opts);

    dbexit( EXIT_CLEAN );
    return 0;
}

#undef exit
void mongo::dbexit( ExitCode rc, const char *why) {
    dbexitCalled = true;
    log() << "dbexit: " << why << " rc:" << rc << endl;
    ::exit(rc);
}
