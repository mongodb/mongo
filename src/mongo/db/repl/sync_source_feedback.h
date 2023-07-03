/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include "mongo/base/status.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {
struct HostAndPort;
class OperationContext;

namespace executor {
class TaskExecutor;
}  // namespace executor

namespace repl {
class BackgroundSync;
class Reporter;

class SyncSourceFeedback {
    SyncSourceFeedback(const SyncSourceFeedback&) = delete;
    SyncSourceFeedback& operator=(const SyncSourceFeedback&) = delete;

public:
    SyncSourceFeedback() = default;

    /// Notifies the SyncSourceFeedbackThread to wake up and send an update upstream of secondary
    /// replication progress.
    void forwardSecondaryProgress();

    /**
     * Loops continuously until shutdown() is called, passing updates when they are present. If no
     * update occurs within the _keepAliveInterval, progress is forwarded to let the upstream node
     * know that this node, along with the alive nodes chaining through it, are still alive.
     *
     * Task executor is used to run replSetUpdatePosition command on sync source.
     */
    void run(executor::TaskExecutor* executor,
             BackgroundSync* bgsync,
             ReplicationCoordinator* replCoord);

    /// Signals the run() method to terminate.
    void shutdown();

private:
    /* Inform the sync target of our current position in the oplog, as well as the positions
     * of all secondaries chained through us.
     */
    Status _updateUpstream(Reporter* reporter);

    // protects cond, _shutdownSignaled, _keepAliveInterval, and _positionChanged.
    Mutex _mtx = MONGO_MAKE_LATCH("SyncSourceFeedback::_mtx");
    // used to alert our thread of changes which need to be passed up the chain
    stdx::condition_variable _cond;
    // used to indicate a position change which has not yet been pushed along
    bool _positionChanged = false;
    // Once this is set to true the _run method will terminate
    bool _shutdownSignaled = false;
    // Reports replication progress to sync source.
    Reporter* _reporter = nullptr;
};

}  // namespace repl
}  // namespace mongo
