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

#include "stdafx.h"
#include "../util/message.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../util/message_server.h"

#include "server.h"
#include "request.h"
#include "config.h"
#include "chunk.h"

namespace mongo {

    int port = 27017;
    const char *curNs = "";
    Database *database = 0;
    string ourHostname;
    OID serverID;
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
        out() << " -v+  verbose\n";
        out() << " --port <portno>\n";
        out() << " --configdb <configdbname> [<configdbname>...]\n";
//        out() << " --infer                                   infer configdbname by replacing \"-n<n>\"\n";
//        out() << "                                           in our hostname with \"-grid\".\n";
        out() << endl;
    }

    class ShardingConnectionHook : public DBConnectionHook {
    public:
        virtual void onCreate( DBClientBase * conn ){
            conn->simpleCommand( "admin" , 0 , "switchtoclienterrors" );
        }
        virtual void onHandedOut( DBClientBase * conn ){
            ClientInfo::get()->addShard( conn->getServerAddress() );
        }
    } shardingConnectionHook;
    
    class ShardedMessageHandler : public MessageHandler {
    public:
        virtual ~ShardedMessageHandler(){}
        virtual void process( Message& m , AbstractMessagingPort* p ){
            Request r( m , p );
            if ( logLevel > 5 ){
                log(5) << "client id: " << hex << r.getClientId() << "\t" << r.getns() << "\t" << dec << r.op() << endl;
            }
            try {
                setClientId( r.getClientId() );
                r.process();
            }
            catch ( DBException& e ){
                log() << "UserException: " << e.what() << endl;
                if ( r.expectResponse() ){
                    BSONObj err = BSON( "$err" << e.what() );
                    replyToQuery( QueryResult::ResultFlag_ErrSet, p , m , err );
                }
            }
        }
    };
    
    void init(){
        serverID.init();
    }

    void start() {
        log() << "waiting for connections on port " << port << endl;
        //DbGridListener l(port);
        //l.listen();
        ShardedMessageHandler handler;
        MessageServer * server = createServer( port , &handler );
        server->run();
    }

    DBClientBase *createDirectClient(){
        uassert( "createDirectClient not implemented for sharding yet" , 0 );
        return 0;
    }

} // namespace mongo

using namespace mongo;

int main(int argc, char* argv[], char *envp[] ) {
    
    bool justTests = false;
    bool infer = false;
    vector<string> configdbs;
    
    for (int i = 1; i < argc; i++)  {
        if ( argv[i] == 0 ) continue;
        string s = argv[i];
        if ( s == "--port" ) {
            port = atoi(argv[++i]);
        }
        else if ( s == "--infer" ) {
            infer = true;
        }
        else if ( s == "--configdb" ) {
            assert( ! infer );
            
            while ( ++i < argc ) 
                configdbs.push_back(argv[i]);

            if ( configdbs.size() == 0 ) {
                out() << "error: no args for --configdb\n";
                return 4;
            }
            
            if ( configdbs.size() > 2 ) {
                out() << "error: --configdb does not support more than 2 parameters yet\n";
                return 5;
            }
        }
        else if ( s.find( "-v" ) == 0 ){
            logLevel = s.size() - 1;
        }
        else if ( s == "--test" ) {
            justTests = true;
            logLevel = 5;
        }
        else {
            usage( argv );
            return 3;
        }
    }
    
    UnitTest::runTests();
    if ( justTests ){
        cout << "tests passed" << endl;
        return 0;
    }
    
    pool.addHook( &shardingConnectionHook );

    if ( argc <= 1 ) {
        usage( argv );
        return 3;
    }

    bool ok = port != 0;
    
    if ( !ok ) {
        usage( argv );
        return 1;
    }

    log() << argv[0] << " v0.3- (alpha 3) starting (--help for usage)" << endl;
    printGitVersion();
    printSysInfo();

    if ( ! configServer.init( configdbs , infer ) ){
        cerr << "couldn't connectd to config db" << endl;
        return 7;
    }
    
    assert( configServer.ok() );
    
    int configError = configServer.checkConfigVersion();
    if ( configError ){
        cerr << "config server error: " << configError << endl;
        return configError;
    }

    init();
    start();
    dbexit( EXIT_CLEAN );
    return 0;
}

#undef exit
void mongo::dbexit( ExitCode rc, const char *why) {
    dbexitCalled = true;
    log() << "dbexit: " << why << " rc:" << rc << endl;
    ::exit(rc);
}
