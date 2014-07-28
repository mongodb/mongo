/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/repl/oplogreader.h"
#include "mongo/client/constants.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/util/background.h"
#include "mongo/util/log.h"

namespace mongo {

    class OperationContext;

namespace repl {

    class Member;

    class SyncSourceFeedback : public BackgroundJob {
    public:
        SyncSourceFeedback() : BackgroundJob(false /*don't selfdelete*/),
                              _syncTarget(NULL),
                              _positionChanged(false),
                              _handshakeNeeded(false) {}

        ~SyncSourceFeedback() {}

        /// Ensures local.me is populated and populates it if not.
        /// TODO(spencer): Remove this function once the LegacyReplicationCoordinator is gone.
        void ensureMe(OperationContext* txn);

        /// Notifies the SyncSourceFeedbackThread to wake up and send a handshake up the replication
        /// chain, upon receiving a handshake.
        void forwardSlaveHandshake();

        /// Notifies the SyncSourceFeedbackThread to wake up and send an update upstream of slave
        /// replication progress.
        void forwardSlaveProgress();

        std::string name() const { return "SyncSourceFeedbackThread"; }

        /// Returns the RID for this process.  ensureMe() must have been called before this can be.
        /// TODO(spencer): Remove this function once the LegacyReplicationCoordinator is gone.
        OID getMyRID() const { return _me["_id"].OID(); }

        /// Loops forever, passing updates when they are present.
        void run();

    private:
        void _resetConnection() {
            MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

            LOG(1) << "resetting connection in sync source feedback";
            _connection.reset();
        }

        /**
         * Authenticates _connection using the server's cluster-membership credentials.
         *
         * Returns true on successful authentication.
         */
        bool replAuthenticate();

        /* Sends initialization information to our sync target, also determines whether or not they
         * support the updater command.
         */
        bool replHandshake(OperationContext* txn);

        /* Inform the sync target of our current position in the oplog, as well as the positions
         * of all secondaries chained through us.
         */
        bool updateUpstream(OperationContext* txn);

        bool hasConnection() {
            return _connection.get();
        }

        /// Connect to sync target.
        bool _connect(OperationContext* txn, const std::string& hostName);

        // stores our OID to be passed along in commands
        /// TODO(spencer): Remove this once the LegacyReplicationCoordinator is gone.
        BSONObj _me;
        // the member we are currently syncing from
        const Member* _syncTarget;
        // our connection to our sync target
        boost::scoped_ptr<DBClientConnection> _connection;
        // protects cond and the indicator bools
        boost::mutex _mtx;
        // used to alert our thread of changes which need to be passed up the chain
        boost::condition _cond;
        // used to indicate a position change which has not yet been pushed along
        bool _positionChanged;
        // used to indicate a connection change which has not yet been shook on
        bool _handshakeNeeded;
    };
} // namespace repl
} // namespace mongo
