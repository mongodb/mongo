// griddb.cpp

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
#include "database.h"
#include "../client/connpool.h"
#include "../db/pdfile.h"
#include "gridconfig.h"
#include "../client/model.h"
#include "../util/background.h"

static boost::mutex griddb_mutex;
GridDB gridDB;
DBClientCommands *Model::globalConn = &gridDB.conn;
string ourHostname;

GridDB::GridDB() { }

void GridDB::init() {
    char buf[256];
    int ec = gethostname(buf, 127);
    if( ec || *buf == 0 ) { 
        log() << "can't get this server's hostname errno:" << ec << endl;
        sleepsecs(5);
        exit(16);
    }
    ourHostname = buf;

    const int DEBUG = 1;
    if( DEBUG ) {
        cout << "TEMPZ DEBUG mode on not for production" << endl;
        strcpy(buf, "iad-sb-n13.10gen.cc");
    }

    char *p = strchr(buf, '-');
    if( p ) 
        p = strchr(p+1, '-');
    if( !p ) {
        log() << "can't parse server's hostname, expect <acode>-<loc>-n<nodenum>, got: " << buf << endl;
        sleepsecs(5);
        exit(17);
    }
    p[1] = 0;

    stringstream sl, sr;
    sl << buf << "grid-l";
    sr << buf << "grid-r"; 
    string hostLeft = sl.str();
    string hostRight = sr.str();
    sl << ":" << Port;
    sr << ":" << Port;
    string left = sl.str();
    string right = sr.str();

    if( DEBUG ) { 
        left = "1.2.3.4"; //"iad-sb-n7.10gen.cc";
        right = "1.2.3.4"; //"iad-sb-n7.10gen.cc";
    }
    else
    /* this loop is not really necessary, we we print out if we can't connect 
       but it gives much prettier error msg this way if the config is totally 
       wrong so worthwhile. 
       */
    while( 1 ) {
        if( hostbyname(hostLeft.c_str()).empty() ) { 
            log() << "can't resolve DNS for " << hostLeft << ", sleeping and then trying again" << endl;
            sleepsecs(15);
            continue;
        }
        if( hostbyname(hostRight.c_str()).empty() ) { 
            log() << "can't resolve DNS for " << hostRight << ", sleeping and then trying again" << endl;
            sleepsecs(15);
            continue;
        }
        break;
    }

    Logstream& l = log();
    (l << "connecting to griddb L:" << left << " R:" << right << "...").flush();

    bool ok = conn.connect(left.c_str(),right.c_str());
    if( !ok ) {
        l << '\n';
        log() << "  griddb connect failure at startup (will retry)" << endl;
    } else {
        l << "ok" << endl;
    }
}
