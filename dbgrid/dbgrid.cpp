// dbgrid.cpp

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
#include "../grid/message.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "gridconfig.h"

bool dashDashInfer = false;
vector<string> dashDashGridDb;
int port = 27017;
const char *curNs = "";
Database *database = 0;

string getDbContext() {
    return "?";
}

/* this is a good place to set a breakpoint when debugging, as lots of warning things
   (assert, wassert) call it.
*/
void sayDbContext(const char *errmsg) {
    if ( errmsg )
        problem() << errmsg << endl;
    printStackTrace();
}

#if !defined(_WIN32)

#include <signal.h>

void pipeSigHandler( int signal ) {
    psignal( signal, "Signal Received : ");
}

#else
void setupSignals() {}
#endif

void usage() {
    cout << "Mongo dbgrid usage:\n\n";
    cout << " --port <portno>\n";
    cout << " --griddb <griddbname> [<griddbname>...]\n";
    cout << " --infer                                   infer griddbname by replacing \"-n<n>\"\n";
    cout << "                                           in our hostname with \"-grid\".\n";
    cout << endl;
}

MessagingPort *grab = 0;
void processRequest(Message&, MessagingPort&);

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
    gridDatabase.init();
    /*
        try {
    cout << "TEMP" << endl;
    {
        ScopedDbConnection c("localhost");
        cout << c.conn().findOne("dwight.bar", emptyObj).toString() << endl;
        c.done();
        cout << "OK1" << endl;
    }
    {
        ScopedDbConnection c("localhost");
        c.conn().findOne("dwight.bar", emptyObj);
        c.done();
        cout << "OK1" << endl;
    }
    cout << "OK2" << endl;
        } catch(...) {
    cout << "exception" << endl;
        }
    */

    log() << "waiting for connections on port " << port << "..." << endl;
    DbGridListener l(port);
    l.listen();
}

int main(int argc, char* argv[], char *envp[] ) {
#if !defined(_WIN32)
    signal(SIGPIPE, pipeSigHandler);
#endif

    if ( argc <= 1 ) {
        usage();
        return 3;
    }

    for (int i = 1; i < argc; i++)  {
        if ( argv[i] == 0 ) continue;
        string s = argv[i];
        if ( s == "--port" ) {
            port = atoi(argv[++i]);
        }
        else if( s == "--infer" ) { 
            dashDashInfer = true;
        }
        else if ( s == "--griddb" ) {
            assert( !dashDashInfer );
            int n = 0;
            while( ++i < argc ) {
                dashDashGridDb.push_back(argv[i]);
                n++;
            }
            if( n == 0 ) {
                cout << "error: no args for --griddb\n";
                return 4;
            }
            if( n > 2 ) { 
                cout << "error: --griddb does not support more than 2 parameters yet\n";
                return 5;
            }
        }
        else {
            usage();
            return 3;
        }
    }

    bool ok = port != 0;

    if ( !ok ) {
        usage();
        return 1;
    }

    log() << "dbgrid starting (--help for usage)" << endl;
    UnitTest::runTests();
    start();
    dbexit(0);
    return 0;
}

#undef exit
void dbexit(int rc, const char *why) {
    log() << "dbexit: " << why << " rc:" << rc << endl;
    exit(rc);
}
