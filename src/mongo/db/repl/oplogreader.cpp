/**
*    Copyright (C) 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplogreader.h"

#include <boost/shared_ptr.hpp>
#include <string>

#include "mongo/base/counter.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    using boost::shared_ptr;

namespace repl {

    //number of readers created;
    //  this happens when the source source changes, a reconfig/network-error or the cursor dies
    static Counter64 readersCreatedStats;
    static ServerStatusMetricField<Counter64> displayReadersCreated(
                                                    "repl.network.readersCreated",
                                                    &readersCreatedStats );


    static const BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

    bool replAuthenticate(DBClientBase *conn) {
        if (!getGlobalAuthorizationManager()->isAuthEnabled())
            return true;

        if (!isInternalAuthSet())
            return false;
        return authenticateInternalUser(conn);
    }

    OplogReader::OplogReader() {
        _tailingQueryOptions = QueryOption_SlaveOk;
        _tailingQueryOptions |= QueryOption_CursorTailable | QueryOption_OplogReplay;
        
        /* TODO: slaveOk maybe shouldn't use? */
        _tailingQueryOptions |= QueryOption_AwaitData;

        readersCreatedStats.increment();
    }

    bool OplogReader::connect(const HostAndPort& host) {
        if (conn() == NULL || _host != host) {
            resetConnection();
            _conn = shared_ptr<DBClientConnection>(new DBClientConnection(false,
                                                                          0,
                                                                          tcp_timeout));
            string errmsg;
            if ( !_conn->connect(host, errmsg) ||
                 (getGlobalAuthorizationManager()->isAuthEnabled() &&
                  !replAuthenticate(_conn.get())) ) {

                resetConnection();
                log() << "repl: " << errmsg << endl;
                return false;
            }
            _host = host;
        }
        return true;
    }

    void OplogReader::tailCheck() {
        if( cursor.get() && cursor->isDead() ) {
            log() << "repl: old cursor isDead, will initiate a new one" << std::endl;
            resetCursor();
        }
    }

    void OplogReader::query(const char *ns,
                            Query query,
                            int nToReturn,
                            int nToSkip,
                            const BSONObj* fields) {
        cursor.reset(
            _conn->query(ns, query, nToReturn, nToSkip, fields, QueryOption_SlaveOk).release()
        );
    }

    void OplogReader::tailingQuery(const char *ns, const BSONObj& query, const BSONObj* fields ) {
        verify( !haveCursor() );
        LOG(2) << "repl: " << ns << ".find(" << query.toString() << ')' << endl;
        cursor.reset( _conn->query( ns, query, 0, 0, fields, _tailingQueryOptions ).release() );
    }

    void OplogReader::tailingQueryGTE(const char *ns, OpTime optime, const BSONObj* fields ) {
        BSONObjBuilder gte;
        gte.appendTimestamp("$gte", optime.asDate());
        BSONObjBuilder query;
        query.append("ts", gte.done());
        tailingQuery(ns, query.done(), fields);
    }

    HostAndPort OplogReader::getHost() const {
        return _host;
    }

    void OplogReader::connectToSyncSource(OperationContext* txn,
                                          OpTime lastOpTimeFetched,
                                          ReplicationCoordinator* replCoord) {
        const OpTime sentinel(Milliseconds(curTimeMillis64()).total_seconds(), 0);
        OpTime oldestOpTimeSeen = sentinel;

        invariant(conn() == NULL);

        while (true) {
            HostAndPort candidate = replCoord->chooseNewSyncSource();

            if (candidate.empty()) {
                if (oldestOpTimeSeen == sentinel) {
                    // If, in this invocation of connectToSyncSource(), we did not successfully 
                    // connect to any node ahead of us,
                    // we apparently have no sync sources to connect to.
                    // This situation is common; e.g. if there are no writes to the primary at
                    // the moment.
                    return;
                }

                // Connected to at least one member, but in all cases we were too stale to use them
                // as a sync source.
                log() << "replSet error RS102 too stale to catch up";
                log() << "replSet our last optime : " << lastOpTimeFetched.toStringLong();
                log() << "replSet oldest available is " << oldestOpTimeSeen.toStringLong();
                log() << "replSet "
                    "See http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember";
                setMinValid(txn, oldestOpTimeSeen);
                bool worked = replCoord->setFollowerMode(MemberState::RS_RECOVERING);
                if (!worked) {
                    warning() << "Failed to transition into "
                              << MemberState(MemberState::RS_RECOVERING)
                              << ". Current state: " << replCoord->getMemberState();
                }
                return;
            }

            if (!connect(candidate)) {
                LOG(2) << "replSet can't connect to " << candidate.toString() << 
                    " to read operations";
                resetConnection();
                replCoord->blacklistSyncSource(candidate, Date_t(curTimeMillis64() + 10*1000));
                continue;
            }
            // Read the first (oldest) op and confirm that it's not newer than our last
            // fetched op. Otherwise, we have fallen off the back of that source's oplog.
            BSONObj remoteOldestOp(findOne(rsoplog, Query()));
            BSONElement tsElem(remoteOldestOp["ts"]);
            if (tsElem.type() != Timestamp) {
                // This member's got a bad op in its oplog.
                warning() << "oplog invalid format on node " << candidate.toString();
                resetConnection();
                replCoord->blacklistSyncSource(candidate, 
                                               Date_t(curTimeMillis64() + 600*1000));
                continue;
            }
            OpTime remoteOldOpTime = tsElem._opTime();

            if (lastOpTimeFetched < remoteOldOpTime) {
                // We're too stale to use this sync source.
                resetConnection();
                replCoord->blacklistSyncSource(candidate, 
                                               Date_t(curTimeMillis64() + 600*1000));
                if (oldestOpTimeSeen > remoteOldOpTime) {
                    warning() << "we are too stale to use " << candidate.toString() << 
                        " as a sync source";
                    oldestOpTimeSeen = remoteOldOpTime;
                }
                continue;
            }

            // Got a valid sync source.
            return;
        } // while (true)
    }

} // namespace repl
} // namespace mongo
