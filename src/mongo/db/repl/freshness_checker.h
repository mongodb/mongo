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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/optime.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"

namespace mongo {

    class Status;

namespace repl {

    class ReplicaSetConfig;
    class ScatterGatherRunner;

    class FreshnessChecker {
        MONGO_DISALLOW_COPYING(FreshnessChecker);
    public:
        class Algorithm : public ScatterGatherAlgorithm {
        public:
            Algorithm(OpTime lastOpTimeApplied,
                      const ReplicaSetConfig& rsConfig,
                      int selfIndex,
                      const std::vector<HostAndPort>& targets);
            virtual ~Algorithm();
            virtual std::vector<ReplicationExecutor::RemoteCommandRequest> getRequests() const;
            virtual void processResponse(
                    const ReplicationExecutor::RemoteCommandRequest& request,
                    const ResponseStatus& response);
            virtual bool hasReceivedSufficientResponses() const;

            bool isFreshest() const { return _freshest; }
            bool isTiedForFreshest() const { return _tied; }

        private:
            // Number of responses received so far.
            size_t _actualResponses;

            // Does this node have the latest applied optime of all queriable nodes in the set?
            bool _freshest;

            // Does this node have the same optime as another queriable node in the set?
            bool _tied;

            // Last OpTime applied by the caller; used in the Fresh command 
            const OpTime _lastOpTimeApplied;

            const ReplicaSetConfig _rsConfig;

            const int _selfIndex;

            const std::vector<HostAndPort> _targets;
        };

        FreshnessChecker();
        virtual ~FreshnessChecker();

        /**
         * Begins the process of sending replSetFresh commands to all non-DOWN nodes
         * in currentConfig, with the intention of determining whether the current node
         * is freshest.
         * evh can be used to schedule a callback when the process is complete.
         * This function must be run in the executor, as it must be synchronous with the command
         * callbacks that it schedules.
         * If this function returns Status::OK(), evh is then guaranteed to be signaled.
         **/
        StatusWith<ReplicationExecutor::EventHandle> start(
            ReplicationExecutor* executor,
            const OpTime& lastOpTimeApplied,
            const ReplicaSetConfig& currentConfig,
            int selfIndex,
            const std::vector<HostAndPort>& targets,
            const stdx::function<void ()>& onCompletion = stdx::function<void ()>());

        /**
         * Informs the freshness checker to cancel further processing.  The "executor"
         * argument must point to the same executor passed to "start()".
         *
         * Like start(), this method must run in the executor context.
         */
        void cancel(ReplicationExecutor* executor);

        /**
         * Returns true if cancel() was called on this instance.
         */
        bool isCanceled() const { return _isCanceled; }

        /**
         * Returns whether this node is the freshest of all non-DOWN nodes in the set,
         * and if any election attempts may be tying because our optime matches another's.
         * Only valid to call after the event handle supplied to start() has been signaled, which 
         * guarantees that the data members will no longer be touched by callbacks.
         */
        void getResults(bool* freshest, bool* tied) const;

        /**
         * Returns the config version supplied in the config when start() was called.
         * Useful for determining if the the config version has changed.
         */
        long long getOriginalConfigVersion() const;

    private:
        boost::scoped_ptr<Algorithm> _algorithm;
        boost::scoped_ptr<ScatterGatherRunner> _runner;
        long long _originalConfigVersion;
        bool _isCanceled;
    };

}  // namespace repl
}  // namespace mongo
