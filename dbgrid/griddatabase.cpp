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
#include "../client/connpool.h"
#include "../db/pdfile.h"
#include "../client/model.h"
#include "../util/background.h"
#include "griddatabase.h"

static boost::mutex griddb_mutex;
GridDatabase gridDatabase;
DBClientWithCommands *Model::globalConn = &gridDatabase.conn;
string ourHostname;
extern string dashDashGridDb;

GridDatabase::GridDatabase() { }

void GridDatabase::init() {
    string hn = getHostName();
    if( hn.empty() ) { 
        sleepsecs(5);
        exit(16);
    }
    ourHostname = hn;

    char buf[256];
    strcpy(buf, hn.c_str());

    if( dashDashGridDb.empty() ) {
        char *p = strchr(buf, '-');
        if( p ) 
            p = strchr(p+1, '-');
        if( !p ) {
            log() << "can't parse server's hostname, expect <city>-<locname>-n<nodenum>, got: " << buf << endl;
            sleepsecs(5);
            exit(17);
        }
        p[1] = 0;
    }

    string left, right; // with :port#
    string hostLeft, hostRight;

    if( dashDashGridDb.empty() ) {
        stringstream sl, sr;
        sl << buf << "grid-l";
        sr << buf << "grid-r"; 
        hostLeft = sl.str();
        hostRight = sr.str();
        sl << ":" << Port;
        sr << ":" << Port;
        left = sl.str();
        right = sr.str();
    }
    else { 
        stringstream sl, sr;
        sl << dashDashGridDb;
        sr << dashDashGridDb;
        if( !isdigit(dashDashGridDb[0]) ) { 
            sl << "-l";
            sr << "-r";
        }
        else { 
            /* ip address specified, so "-l" / "-r" not meaningful 
               silly though that we put it on both sides -- to be fixed.
            */
        }
        hostLeft = sl.str();
        hostRight = sr.str();
        sl << ":" << Port;
        sr << ":" << Port;
        left = sl.str();
        right = sr.str();
    }


    if( !isdigit(left[0]) )
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
