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
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../oplogreader.h"
#include "../../util/mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    void dropAllDatabasesExceptLocal();

    // add try/catch with sleep

    void isyncassert(const char *msg, bool expr) { 
        if( !expr ) { 
            string m = str::stream() << "initial sync " << msg;
            theReplSet->sethbmsg(m, 0);
            uasserted(13388, m);
        }
    }

    void ReplSetImpl::syncDoInitialSync() { 
        while( 1 ) {
            try {
                _syncDoInitialSync();
                break;
            }
            catch(DBException&) {
                log(1) << "replSet initial sync exception; sleep 30 sec" << rsLog;
                sleepsecs(30);
            }
        }
    }

    void ReplSetImpl::_syncDoInitialSync() { 
        sethbmsg("initial sync pending");

        assert( !isPrimary() ); // wouldn't make sense if we were.

        const Member *cp = currentPrimary();
        if( cp == 0 ) {
            sethbmsg("initial sync need a member to be primary");
            sleepsecs(15);
            return;
        }

        OplogReader r;
        if( !r.connect(cp->h().toString()) ) {
            sethbmsg( str::stream() << "initial sync couldn't connect to " << cp->h().toString() );
            sleepsecs(15);
            return;
        }

        BSONObj lastOp = r.getLastOp(rsoplog);
        OpTime ts = lastOp["ts"]._opTime();
        
        {
            /* make sure things aren't too flappy */
            sleepsecs(5);
            isyncassert( "flapping?", currentPrimary() == cp );
        }

        sethbmsg("initial sync drop all databases");
        dropAllDatabasesExceptLocal();
        sethbmsg("initial sync - not yet implemented");

        assert( !isPrimary() ); // wouldn't make sense if we were.
    }

}
