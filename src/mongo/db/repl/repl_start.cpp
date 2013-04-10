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

#include "mongo/db/repl/repl_start.h"

#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <iostream>

#include "mongo/db/cmdline.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/log.h"

namespace mongo {

    void startReplication() {
        /* if we are going to be a replica set, we aren't doing other forms of replication. */
        if( !cmdLine._replSet.empty() ) {
            if( replSettings.slave || replSettings.master ) {
                log() << "***" << endl;
                log() << "ERROR: can't use --slave or --master replication options with --replSet" << endl;
                log() << "***" << endl;
            }
            newRepl();

            replSet = true;
            ReplSetCmdline *replSetCmdline = new ReplSetCmdline(cmdLine._replSet);
            boost::thread t( boost::bind( &startReplSets, replSetCmdline) );

            return;
        }

        startMasterSlave();
    }

} // namespace mongo
