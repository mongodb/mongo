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
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/executor/network_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::endl;
using std::string;

namespace repl {

const BSONObj reverseNaturalObj = BSON("$natural" << -1);

// number of readers created;
//  this happens when the source source changes, a reconfig/network-error or the cursor dies
static Counter64 readersCreatedStats;
static ServerStatusMetricField<Counter64> displayReadersCreated("repl.network.readersCreated",
                                                                &readersCreatedStats);


bool replAuthenticate(DBClientBase* conn) {
    if (isInternalAuthSet())
        return conn->authenticateInternalUser();
    if (getGlobalAuthorizationManager()->isAuthEnabled())
        return false;
    return true;
}

const Seconds OplogReader::kSocketTimeout(30);

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
        _conn = shared_ptr<DBClientConnection>(
            new DBClientConnection(false, durationCount<Seconds>(kSocketTimeout)));
        string errmsg;
        if (!_conn->connect(host, errmsg) || !replAuthenticate(_conn.get())) {
            resetConnection();
            error() << errmsg << endl;
            return false;
        }
        _conn->port().setTag(_conn->port().getTag() |
                             executor::NetworkInterface::kMessagingPortKeepOpen);
        _host = host;
    }
    return true;
}

void OplogReader::tailCheck() {
    if (cursor.get() && cursor->isDead()) {
        log() << "old cursor isDead, will initiate a new one" << std::endl;
        resetCursor();
    }
}

void OplogReader::query(
    const char* ns, Query query, int nToReturn, int nToSkip, const BSONObj* fields) {
    cursor.reset(
        _conn->query(ns, query, nToReturn, nToSkip, fields, QueryOption_SlaveOk).release());
}

void OplogReader::tailingQuery(const char* ns, const BSONObj& query) {
    verify(!haveCursor());
    LOG(2) << ns << ".find(" << query.toString() << ')' << endl;
    cursor.reset(_conn->query(ns, query, 0, 0, nullptr, _tailingQueryOptions).release());
}

void OplogReader::tailingQueryGTE(const char* ns, Timestamp optime) {
    BSONObjBuilder gte;
    gte.append("$gte", optime);
    BSONObjBuilder query;
    query.append("ts", gte.done());
    tailingQuery(ns, query.done());
}

HostAndPort OplogReader::getHost() const {
    return _host;
}

void OplogReader::connectToSyncSource(OperationContext* txn,
                                      const OpTime& lastOpTimeFetched,
                                      ReplicationCoordinator* replCoord) {
    const Timestamp sentinelTimestamp(duration_cast<Seconds>(Date_t::now().toDurationSinceEpoch()),
                                      0);
    const OpTime sentinel(sentinelTimestamp, std::numeric_limits<long long>::max());
    OpTime oldestOpTimeSeen = sentinel;

    invariant(conn() == NULL);

    while (true) {
        HostAndPort candidate = replCoord->chooseNewSyncSource(lastOpTimeFetched.getTimestamp());

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
            error() << "too stale to catch up -- entering maintenance mode";
            log() << "our last optime : " << lastOpTimeFetched;
            log() << "oldest available is " << oldestOpTimeSeen;
            log() << "See http://dochub.mongodb.org/core/resyncingaverystalereplicasetmember";
            StorageInterface::get(txn)->setMinValid(txn, {lastOpTimeFetched, oldestOpTimeSeen});
            auto status = replCoord->setMaintenanceMode(true);
            if (!status.isOK()) {
                warning() << "Failed to transition into maintenance mode.";
            }
            bool worked = replCoord->setFollowerMode(MemberState::RS_RECOVERING);
            if (!worked) {
                warning() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING)
                          << ". Current state: " << replCoord->getMemberState();
            }
            return;
        }

        if (!connect(candidate)) {
            LOG(2) << "can't connect to " << candidate.toString() << " to read operations";
            resetConnection();
            replCoord->blacklistSyncSource(candidate, Date_t::now() + Seconds(10));
            continue;
        }
        // Read the first (oldest) op and confirm that it's not newer than our last
        // fetched op. Otherwise, we have fallen off the back of that source's oplog.
        BSONObj remoteOldestOp(findOne(rsOplogName.c_str(), Query()));
        OpTime remoteOldOpTime =
            fassertStatusOK(28776, OpTime::parseFromOplogEntry(remoteOldestOp));

        // remoteOldOpTime may come from a very old config, so we cannot compare their terms.
        if (!lastOpTimeFetched.isNull() &&
            lastOpTimeFetched.getTimestamp() < remoteOldOpTime.getTimestamp()) {
            // We're too stale to use this sync source.
            resetConnection();
            replCoord->blacklistSyncSource(candidate, Date_t::now() + Minutes(1));
            if (oldestOpTimeSeen.getTimestamp() > remoteOldOpTime.getTimestamp()) {
                warning() << "we are too stale to use " << candidate.toString()
                          << " as a sync source";
                oldestOpTimeSeen = remoteOldOpTime;
            }
            continue;
        }


        // TODO: If we were too stale (recovering with maintenance mode on), then turn it off, to
        //       allow becoming secondary/etc.

        // Got a valid sync source.
        return;
    }  // while (true)
}

}  // namespace repl
}  // namespace mongo
