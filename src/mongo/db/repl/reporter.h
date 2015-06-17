/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace repl {

    class ReplicationProgressManager {
        public:
            virtual bool prepareReplSetUpdatePositionCommand(BSONObjBuilder* cmdBuilder) = 0;
            virtual ~ReplicationProgressManager();
    };

    class Reporter {
        MONGO_DISALLOW_COPYING(Reporter);

    public:
        Reporter(ReplicationExecutor* executor,
                 ReplicationProgressManager* ReplicationProgressManager,
                 const HostAndPort& target);
        virtual ~Reporter();

        /**
         * Returns true if a remote command has been scheduled (but not completed)
         * with the executor.
         */
        bool isActive() const;

        /**
         * Returns true if a remote command should be scheduled once the current one returns 
         * from the executor.
         */
        bool willRunAgain() const;

        /**
         * Cancels remote command request.
         * Returns immediately if the Reporter is not active.
         */
        void cancel();

        /**
         * Waits for last/current executor handle to finish.
         * Returns immediately if the handle is invalid.
         */
        void wait();

        /**
         * Signals to the Reporter that there is new information to be sent to the "_target" server.
         * Returns the _status, indicating any error the Reporter has encountered.
         */
        Status trigger();

        /**
         * Returns the previous return status so that the owner can decide whether the Reporter
         * needs a new target to whom it can report.
         */
        Status getStatus() const;

    private:
        /**
         * Schedules remote command to be run by the executor
         */
        Status _schedule_inlock();

        /**
         * Callback for remote command.
         */
        void _callback(const ReplicationExecutor::RemoteCommandCallbackArgs& rcbd);

        // Not owned by us.
        ReplicationExecutor* _executor;
        ReplicationProgressManager* _updatePositionSource;

        // Host to whom the Reporter sends updates.
        HostAndPort _target;

        // Protects member data of this Reporter.
        mutable stdx::mutex _mutex;

        // Stores the most recent Status returned from the ReplicationExecutor.
        Status _status;

        // _willRunAgain is true when Reporter is scheduled to be run by the executor and subsequent
        // updates have come in.
        bool _willRunAgain;
        // _active is true when Reporter is scheduled to be run by the executor.
        bool _active;

        // Callback handle to the scheduled remote command.
        ReplicationExecutor::CallbackHandle _remoteCommandCallbackHandle;
    };

} // namespace repl
} // namespace mongo
