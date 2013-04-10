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

#include "mongo/db/repl/repl_reads_ok.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/parsed_query.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/replutil.h"
#include "mongo/util/assert_util.h"

namespace mongo {

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
            // master/slave
            uassert( 10107,
                     "not master", 
                     isMaster() || 
                     (!pq || pq->hasOption(QueryOption_SlaveOk)) ||
                     replSettings.slave == SimpleSlave );
        }
    }

} // namespace mongo
