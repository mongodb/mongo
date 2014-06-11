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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/repl/repl_reads_ok.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

    /** we allow queries to SimpleSlave's */
    void replVerifyReadsOk(const std::string& ns, const LiteParsedQuery* pq) {
        if( replSet ) {
            // todo: speed up the secondary case.  as written here there are 2 mutex entries, it
            // can b 1.
            if (getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                    NamespaceString(ns).db())) return;
            if ( cc().isGod() ) return;

            // TODO(dannenberg) this seems wrong; what if readPref is PRIMARY_ONLY
            uassert(NotMasterNoSlaveOkCode, "not master and slaveOk=false",
                    !pq || pq->hasOption(QueryOption_SlaveOk) || pq->hasReadPref());
            uassert(NotMasterOrSecondaryCode,
                    "not master or secondary; cannot currently read from this replSet member",
                    theReplSet && theReplSet->isSecondary() );
        }
        else {
            // master/slave
            // TODO(dannenberg) this seems wrong; what if it's a master/slave slave without SlaveOk set?
            uassert(NotMaster,
                    "not master",
                    getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                        NamespaceString(ns).db()) ||
                    pq == NULL || 
                    pq->hasOption(QueryOption_SlaveOk) ||
                    replSettings.slave == SimpleSlave );
        }
    }

} // namespace repl
} // namespace mongo
