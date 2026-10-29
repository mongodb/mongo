// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo {
struct HostAndPort;
class OperationContext;

namespace executor {
class TaskExecutor;
}  // namespace executor

namespace repl {
class BackgroundSync;
class Reporter;

class [[MONGO_MOD_PARENT_PRIVATE]] SyncSourceFeedback {
    SyncSourceFeedback(const SyncSourceFeedback&) = delete;
    SyncSourceFeedback& operator=(const SyncSourceFeedback&) = delete;

public:
    SyncSourceFeedback() = default;

    /// Notifies the SyncSourceFeedbackThread to wake up and send an update upstream of secondary
    /// replication progress.
    void forwardSecondaryProgress(bool prioritized = false);

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
    std::mutex _mtx;
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
