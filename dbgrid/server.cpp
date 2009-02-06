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

#include "server.h"
#include "configserver.h"
#include "gridconfig.h"

namespace mongo {

    int port = 27017;
    const char *curNs = "";
    Database *database = 0;
    DBClientWithCommands* Model::globalConn;
    string ourHostname;

    string getDbContext() {
        return "?";
    }

    void usage( char * argv[] ){
        out() << argv[0] << " usage:\n\n";
        out() << " -v+  verbose\n";
        out() << " --port <portno>\n";
        out() << " --configdb <configdbname> [<configdbname>...]\n";
        out() << " --infer                                   infer configdbname by replacing \"-n<n>\"\n";
        out() << "                                           in our hostname with \"-grid\".\n";
        out() << endl;
    }

    MessagingPort *grab = 0;

    void _dbGridConnThread() {
        MessagingPort& dbMsgPort = *grab;
        grab = 0;
        Message m;
        while ( 1 ) {
            m.reset();

            if ( !dbMsgPort.recv(m) ) {
                log() << "end connection " << dbMsgPort.farEnd.toString() << endl;
                dbMsgPort.shutdown();
                break;
            }

            processRequest(m, dbMsgPort);
        }

    }

    void dbGridConnThread() {
        MessagingPort *p = grab;
        try {
            _dbGridConnThread();
        } catch ( ... ) {
            problem() << "uncaught exception in dbgridconnthread, closing connection" << endl;
            delete p;
        }
    }

    class DbGridListener : public Listener {
    public:
        DbGridListener(int p) : Listener(p) { }
        virtual void accepted(MessagingPort *mp) {
            assert( grab == 0 );
            grab = mp;
            boost::thread thr(dbGridConnThread);
            while ( grab )
                sleepmillis(1);
        }
    };

    void start() {
        log() << "waiting for connections on port " << port << "..." << endl;
        DbGridListener l(port);
        l.listen();
    }

} // namespace mongo

using namespace mongo;

int main(int argc, char* argv[], char *envp[] ) {
    
    if ( argc <= 1 ) {
        usage( argv );
        return 3;
    }

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
        else {
            usage( argv );
            return 3;
        }
    }
    
    bool ok = port != 0;
    
    if ( !ok ) {
        usage( argv );
        return 1;
    }

    log() << argv[0] << " starting (--help for usage)" << endl;
    UnitTest::runTests();

    if ( ! configServer.init( configdbs , infer ) ){
        cerr << "couldn't connectd to config db" << endl;
        return 7;
    }

    assert( configServer.ok() );
    Model::globalConn = 0;

    start();
    dbexit(0);
    return 0;
}

#undef exit
void mongo::dbexit(int rc, const char *why) {
    log() << "dbexit: " << why << " rc:" << rc << endl;
    ::exit(rc);
}
