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
#include "mongo/base/status_with.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <functional>

namespace mongo {
namespace repl {

/**
 * The RollbackChecker maintains a sync source and its baseline rollback ID (rbid). It
 * contains methods to check if a rollback occurred by checking if the rbid has changed since
 * the baseline was set.
 *
 * To use the RollbackChecker:
 * 1) Create a RollbackChecker by passing in an executor, and a sync source to use. If the
 *    sync source changes, make a new RollbackChecker.
 * 2) Call reset(), either synchronously or asynchronously, so that the RollbackChecker retrieves
 *    the state it needs from the sync source to check for rollbacks.
 * 3) Call checkForRollback(), passing in a 'nextAction' callback. This checks if there was a
 *    rollback at the sync source. If there is a rollback or another error, the 'nextAction'
 *    callback will be invoked. This error could be an UnrecoverableRollbackError, when there is a
 *    rollback.
 *    The only case where 'nextAction' is not called is when checkForRollback fails because the
 *    task executor is unable to schedule the remote replSetGetRBID command. In this case,
 *    checkForRollback() returns the error from executorTaskExecutor::scheduleRemoteCommand().
 *    Alternatively, call hasHadRollback() which synchronously checks for a rollback and returns
 *    true if any error occurs. Checking for a rollback does not reset the baseline rbid.
 * 4) Repeat steps 2 and 3 as needed.
 *
 */
class MONGO_MOD_PUB RollbackChecker {
    RollbackChecker(const RollbackChecker&) = delete;
    RollbackChecker& operator=(const RollbackChecker&) = delete;

public:
    // Rollback checker result - true if rollback occurred; false if rollback IDs
    // were the same; Otherwise, error status indicating why rollback check failed.
    using Result = StatusWith<bool>;
    using CallbackFn = std::function<void(const Result& result)>;
    using RemoteCommandCallbackFn = executor::TaskExecutor::RemoteCommandCallbackFn;
    using CallbackHandle = executor::TaskExecutor::CallbackHandle;

    RollbackChecker(executor::TaskExecutor* executor, HostAndPort syncSource);

    virtual ~RollbackChecker();

    // Checks whether the the sync source has had a rollback since the last time reset was called,
    // and then calls the nextAction with the rollback checker result. An error status
    // will be passed to the callback if the RBID cannot be determined or if
    // the callback was canceled.
    StatusWith<CallbackHandle> checkForRollback(const CallbackFn& nextAction);

    // Synchronously checks if there has been a rollback and returns a boolean specifying if one
    // has occurred.
    Result hasHadRollback();

    // Resets the state used to decide if a rollback occurs, and then calls the nextAction with a
    // status specifying what should occur next. The status will either be OK if there was no
    // error or another status if the command failed. The nextAction should account for
    // each of these cases.
    StatusWith<CallbackHandle> reset(const CallbackFn& nextAction);

    // Synchronously calls reset and returns the Status of the command.
    Status reset_sync();

    // Returns the current baseline rbid.
    int getBaseRBID();

    // ================== Test Support API ===================

    // Returns the last rbid seen.
    int getLastRBID_forTest();

private:
    // Assumes a lock has been taken. Returns if a rollback has occurred by comparing the remoteRBID
    // provided and the stored baseline rbid. Sets _lastRBID to the remoteRBID provided.
    bool _checkForRollback(WithLock lk, int remoteRBID);

    // Schedules a remote command to get the rbid at the sync source and then calls the nextAction.
    // If there is an error scheduling the call, it returns the error from
    // TaskExecutor::scheduleRemoteCommand().
    StatusWith<CallbackHandle> _scheduleGetRollbackId(const RemoteCommandCallbackFn& nextAction);

    // Assumes a lock has been taken. Sets the current rbid used as the baseline for rollbacks.
    void _setRBID(WithLock lk, int rbid);

    // Not owned by us.
    executor::TaskExecutor* const _executor;

    // Protects member data of this RollbackChecker.
    mutable stdx::mutex _mutex;

    // The sync source to check for rollbacks against.
    HostAndPort _syncSource;

    // The baseline rbid of the sync source to use when deciding if a rollback should occur.
    int _baseRBID;

    // The last rbid that we saw.
    int _lastRBID;
};

}  // namespace repl
}  // namespace mongo
