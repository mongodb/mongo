/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include <boost/date_time/posix_time/posix_time_types.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_executor.h"

namespace mongo {

    class BSONObj;
    class BSONObjBuilder;
    struct HostAndPort;
    class IndexDescriptor;
    class NamespaceString;
    class OperationContext;
    class OpTime;
    struct WriteConcernOptions;

namespace repl {

    class TopologyCoordinator;

    /**
     * The ReplicationCoordinator is responsible for coordinating the interaction of replication
     * with the rest of the system.  The public methods on ReplicationCoordinator are the public
     * API that the replication subsystem presents to the rest of the codebase.
     */
    class ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(ReplicationCoordinator);

    public:

        typedef boost::posix_time::milliseconds Milliseconds;

        virtual ~ReplicationCoordinator();

        /**
         * Does any initial bookkeeping needed to start replication, and instructs the other
         * components of the replication system to start up whatever threads and do whatever
         * initialization they need.
         * Takes ownership of the passed-in TopologyCoordinator and NetworkInterface.
         */
        virtual void startReplication(TopologyCoordinator* topCoord,
                                      ReplicationExecutor::NetworkInterface* network) = 0;

        /**
         * Does whatever cleanup is required to stop replication, including instructing the other
         * components of the replication system to shut down and stop any threads they are using,
         * blocking until all replication-related shutdown tasks are complete.
         */
        virtual void shutdown() = 0;

        /**
         * Returns true if it is safe to shut down the server now.  Currently the only time this
         * can be false is if this node is primary and there are no secondaries within 10 seconds
         * of our optime.
         */
        virtual bool isShutdownOkay() const = 0;

        enum Mode {
            modeNone = 0,
            modeReplSet,
            modeMasterSlave
        };

        /**
         * Returns a value indicating whether this node is a standalone, master/slave, or replicaset
         */
        virtual Mode getReplicationMode() const = 0;

        /**
         * Returns true if this node is configured to be a member of a replica set or master/slave
         * setup.
         */
        virtual bool isReplEnabled() const { return getReplicationMode() != modeNone; }

        /**
         * Returns the current replica set state of this node (PRIMARY, SECONDARY, STARTUP, etc).
         */
        virtual MemberState getCurrentMemberState() const = 0;

        /**
         * Blocks the calling thread for up to "timeout" millis, or until "ts" has been replicated
         * to at least a set of nodes that satisfies the writeConcern, whichever comes first. Will
         * return a Status with ErrorCodes::ExceededTimeLimit if the timeout is reached before the
         * data has been sufficiently replicated.
         */
        virtual Status awaitReplication(const OpTime& ts,
                                        const WriteConcernOptions& writeConcern,
                                        Milliseconds timeout) = 0;

        /**
         * TODO a way to trigger an action on replication of a given operation
         */
        // handle_t onReplication(OpTime ts, writeConcern, callbackFunction); // TODO

        /**
         * Returns true if it is valid for this node to accept writes on the given database.
         * Currently this is true only if this node is Primary, master in master/slave,
         * a standalone, or is writing to the local database.
         */
        virtual bool canAcceptWritesForDatabase(const StringData& dbName) = 0;

        /**
         * Returns true if it is valid for this node to serve reads on the given collection.
         */
        virtual bool canServeReadsFor(const NamespaceString& collection) = 0;

        /**
         * Returns true if this node should ignore unique index constraints on new documents.
         * Currently this is needed for nodes in STARTUP2, RECOVERING, and ROLLBACK states.
         */
        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx) = 0;

        /**
         * Updates our internal tracking of the last OpTime applied for the given member of the
         * set identified by "member".  Also updates all bookkeeping related to tracking what the
         * last OpTime applied by all tag groups that "member" is a part of.  This is called when
         * secondaries notify the member they are syncing from of their progress in replication.
         * This information is used by awaitReplication to satisfy write concerns.  It is *not*
         * used in elections, we maintain a separate view of member optimes in the topology
         * coordinator based on incoming heartbeat messages, which is used in elections.
         */
        virtual Status setLastOptime(const HostAndPort& member, const OpTime& ts) = 0;

        /**
         * Handles an incoming heartbeat command. Adds BSON to 'resultObj'; 
         * returns a Status with either OK or an error message.
         */
        virtual Status processHeartbeat(const BSONObj& cmdObj, BSONObjBuilder* resultObj) = 0;

    protected:

        ReplicationCoordinator();

    };

} // namespace repl
} // namespace mongo
