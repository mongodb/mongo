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
#include "../repl.h"

namespace mongo {

    void startSyncThread() { 
        Client::initThread("rs_sync");
        theReplSet->syncThread();
    }

    void ReplSetImpl::syncTail() { 
        // todo : locking...
        OplogReader r;
//        r.connect(
    }

    void ReplSetImpl::_syncThread() {
        if( isPrimary() ) 
            return;

        /* later, we can sync from up secondaries if we want. tbd. */
        if( currentPrimary() == 0 )
            return;

        /* do we have anything at all? */
        if( lastOpTimeWritten.isNull() ) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        syncTail();
    }

    void ReplSetImpl::syncThread() {
        while( 1 ) { 
            try {
                _syncThread();
            }
            catch(DBException& e) { 
                log() << "replSet syncThread: " << e.toString() << rsLog;
                sleepsecs(10);
            }
            sleepsecs(2);
        }
    }

}
