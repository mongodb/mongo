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
#include "mongo/bson/oid.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"

namespace mongo {

class Status;

namespace repl {

class ReplicaSetConfig;
class ScatterGatherRunner;

class ElectCmdRunner {
    MONGO_DISALLOW_COPYING(ElectCmdRunner);

public:
    class Algorithm : public ScatterGatherAlgorithm {
    public:
        Algorithm(const ReplicaSetConfig& rsConfig,
                  int selfIndex,
                  const std::vector<HostAndPort>& targets,
                  OID round);

        virtual ~Algorithm();
        virtual std::vector<executor::RemoteCommandRequest> getRequests() const;
        virtual void processResponse(const executor::RemoteCommandRequest& request,
                                     const ResponseStatus& response);
        virtual bool hasReceivedSufficientResponses() const;

        int getReceivedVotes() const {
            return _receivedVotes;
        }

    private:
        // Tally of the number of received votes for this election.
        int _receivedVotes;

        // Number of responses received so far.
        size_t _actualResponses;

        bool _sufficientResponsesReceived;

        const ReplicaSetConfig _rsConfig;
        const int _selfIndex;
        const std::vector<HostAndPort> _targets;
        const OID _round;
    };

    ElectCmdRunner();
    ~ElectCmdRunner();

    /**
     * Begins the process of sending replSetElect commands to all non-DOWN nodes
     * in currentConfig.
     *
     * Returned handle can be used to schedule a callback when the process is complete.
     */
    StatusWith<ReplicationExecutor::EventHandle> start(ReplicationExecutor* executor,
                                                       const ReplicaSetConfig& currentConfig,
                                                       int selfIndex,
                                                       const std::vector<HostAndPort>& targets);

    /**
     * Informs the ElectCmdRunner to cancel further processing.
     */
    void cancel();

    /**
     * Returns the number of received votes.  Only valid to call after
     * the event handle returned from start() has been signaled, which guarantees that
     * the vote count will no longer be touched by callbacks.
     */
    int getReceivedVotes() const;

    /**
     * Returns true if cancel() was called on this instance.
     */
    bool isCanceled() const {
        return _isCanceled;
    }

private:
    std::unique_ptr<Algorithm> _algorithm;
    std::unique_ptr<ScatterGatherRunner> _runner;
    bool _isCanceled;
};
}
}
