// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <mutex>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {

/**
 * Once scheduled, the reporter will periodically send the current replication progress, obtained
 * by invoking "_prepareReplSetUpdatePositionCommandFn", to its sync source until it encounters
 * an error.
 *
 * If the sync source cannot accept the current format (used by server versions 3.2.4 and above) of
 * the "replSetUpdatePosition" command, the reporter will not abort and instead downgrade the format
 * of the command it will send upstream.
 *
 * While the reporter is active, it will be in one of three states:
 * 1) triggered and waiting to send command to sync source as soon as possible.
 * 2) waiting for command response from sync source.
 * 3) waiting for at least "_keepAliveInterval" ms before sending command to sync source.
 *
 * Calling trigger() while the reporter is in state 1 or 2 will cause the reporter to immediately
 * send a new command upon receiving a successful command response.
 *
 * Calling trigger() while it is in state 3 sends a command upstream and cancels the current
 * keep alive timeout, resetting the keep alive schedule.
 */
class Reporter {
    Reporter(const Reporter&) = delete;
    Reporter& operator=(const Reporter&) = delete;

public:
    /**
     * Prepares a BSONObj describing an invocation of the replSetUpdatePosition command that can
     * be sent to this node's sync source to update it about our progress in replication.
     *
     * The returned status indicates whether or not the command was created.
     */
    using PrepareReplSetUpdatePositionCommandFn = std::function<StatusWith<BSONObj>()>;

    Reporter(executor::TaskExecutor* executor,
             PrepareReplSetUpdatePositionCommandFn prepareReplSetUpdatePositionCommandFn,
             const HostAndPort& target,
             Milliseconds keepAliveInterval,
             Milliseconds updatePositionTimeout);

    virtual ~Reporter();

    /**
     * Returns sync target.
     */
    HostAndPort getTarget() const;

    /**
     * Returns an informational string.
     */
    std::string toString() const;

    /**
     * Returns keep alive interval.
     * Reporter will periodically send replication status to sync source every "_keepAliveInterval"
     * until an error occurs.
     */
    Milliseconds getKeepAliveInterval() const;

    /**
     * Returns true if a remote command has been scheduled (but not completed)
     * with the executor.
     */
    bool isActive() const;

    /**
     * Returns true if a remote command has been scheduled (but not completed)
     * with the executor using the backupChannel.
     */
    bool isBackupActive() const;

    /**
     * Returns true if new data is available while a remote command is in progress.
     * The reporter will schedule a subsequent remote update immediately upon successful
     * completion of the previous command instead of when the keep alive callback runs.
     */
    bool isWaitingToSendReport() const;

    /**
     * Cancels both scheduled and active remote command requests.
     * Returns immediately if the Reporter is not active.
     */
    void shutdown();

    /**
     * Waits until Reporter is inactive and returns reporter status.
     */
    Status join();

    /**
     * Signals to the Reporter that there is new information to be sent to the "_target" server.
     * Returns the _status, indicating any error the Reporter has encountered.
     */
    Status trigger(bool allowOneMore = false);

    // ================== Test support API ===================

    /**
     * Returns scheduled time of keep alive timeout handler.
     */
    Date_t getKeepAliveTimeoutWhen_forTest() const;
    Status getStatus_forTest() const;

    enum class RequestWaitingStatus { kNoWaiting, kNormalWaiting, kPrioritizedWaiting };

private:
    /**
     * Returns true if reporter is active.
     */
    bool _isActive(WithLock lk) const;

    /**
     * Returns true if reporter's backup channel is also active.
     */
    bool _isBackupActive(WithLock lk) const;

    /**
     * Prepares remote command to be run by the executor.
     */
    StatusWith<BSONObj> _prepareCommand();

    /**
     * Schedules remote command to be run by the executor with the given network timeout.
     */
    void _sendCommand(WithLock lk,
                      BSONObj commandRequest,
                      Milliseconds netTimeout,
                      bool useBackupChannel);

    /**
     * Callback for processing response from remote command.
     */
    void _processResponseCallback(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd,
                                  bool useBackupChannel);

    /**
     * Callback for preparing and sending remote command.
     */
    void _prepareAndSendCommandCallback(const executor::TaskExecutor::CallbackArgs& args,
                                        bool fromTrigger,
                                        bool useBackupChannel);

    /**
     * Signals end of Reporter work and notifies waiters.
     */
    void _onShutdown(WithLock lk, bool useBackupChannel);

    // Not owned by us.
    executor::TaskExecutor* const _executor;

    // Prepares update command object.
    const PrepareReplSetUpdatePositionCommandFn _prepareReplSetUpdatePositionCommandFn;

    // Host to whom the Reporter sends updates.
    const HostAndPort _target;

    // Reporter will send updates every "_keepAliveInterval" ms until the reporter is canceled or
    // encounters an error.
    const Milliseconds _keepAliveInterval;

    // The network timeout used when sending an updatePosition command to our sync source.
    const Milliseconds _updatePositionTimeout;

    // Protects member data of this Reporter declared below.
    mutable std::mutex _mutex;

    mutable stdx::condition_variable _condition;

    // Stores the most recent Status returned from the executor.
    Status _status = Status::OK();

    // _isWaitingToSendReporter is true when Reporter is scheduled to be run by the executor and
    // subsequent updates have come in.
    RequestWaitingStatus _requestWaitingStatus = RequestWaitingStatus::kNoWaiting;

    // Callback handle to the scheduled remote command.
    executor::TaskExecutor::CallbackHandle _remoteCommandCallbackHandle;

    // Callback handle to the scheduled task for preparing and sending the remote command.
    executor::TaskExecutor::CallbackHandle _prepareAndSendCommandCallbackHandle;

    // Callback handle to the scheduled backup remote command.
    executor::TaskExecutor::CallbackHandle _backupRemoteCommandCallbackHandle;

    // Callback handle to the scheduled backup task for preparing and sending the remote command.
    executor::TaskExecutor::CallbackHandle _backupPrepareAndSendCommandCallbackHandle;

    // Keep alive timeout callback will not run before this time.
    // If this date is Date_t(), the callback is either unscheduled or canceled.
    // Used for testing only.
    Date_t _keepAliveTimeoutWhen;
};

}  // namespace repl
}  // namespace mongo
