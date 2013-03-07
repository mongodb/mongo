// @file repl.cpp

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

#include <boost/thread/thread.hpp>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "jsobj.h"
#include "../util/goodies.h"
#include "repl.h"
#include "../util/net/message.h"
#include "../util/background.h"
#include "../client/connpool.h"
#include "pdfile.h"
#include "db.h"
#include "commands.h"
#include "cmdline.h"
#include "repl_block.h"
#include "repl/rs.h"
#include "replutil.h"
#include "repl/connections.h"
#include "ops/update.h"
#include "pcrecpp.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/instance.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/queryutil.h"
#include "mongo/base/counter.h"
#include "mongo/db/oplog.h"
#include "mongo/db/repl/master_slave.h"

namespace mongo {

    void startReplSets(ReplSetCmdline*);
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

    /** we allow queries to SimpleSlave's */
    void replVerifyReadsOk(const ParsedQuery* pq) {
        if( replSet ) {
            // todo: speed up the secondary case.  as written here there are 2 mutex entries, it
            // can b 1.
            if( isMaster() ) return;
            uassert(13435, "not master and slaveOk=false",
                    !pq || pq->hasOption(QueryOption_SlaveOk) || pq->hasReadPref());
            uassert(13436,
                    "not master or secondary; cannot currently read from this replSet member",
                    theReplSet && theReplSet->isSecondary() );
        }
        else {
            uassert( 10107,
                     "not master", 
                     isMaster() || 
                     (!pq || pq->hasOption(QueryOption_SlaveOk)) ||
                     replSettings.slave == SimpleSlave );
        }
    }

} // namespace mongo
