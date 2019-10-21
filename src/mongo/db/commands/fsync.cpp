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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/commands/fsync.h"

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace {
// Ensures that only one command is operating on fsyncLock state at a time. As a 'ResourceMutex',
// lock time will be reported for a given user operation.
Lock::ResourceMutex commandMutex("fsyncCommandMutex");
}  // namespace

/**
 * Maintains a global read lock while mongod is fsyncLocked.
 */
class FSyncLockThread : public BackgroundJob {
public:
    FSyncLockThread(bool allowFsyncFailure)
        : BackgroundJob(false), _allowFsyncFailure(allowFsyncFailure) {}
    virtual ~FSyncLockThread() {}
    virtual string name() const {
        return "FSyncLockThread";
    }
    virtual void run();

private:
    bool _allowFsyncFailure;
    static bool _shutdownTaskRegistered;
};

class FSyncCommand : public ErrmsgCommandDeprecated {
public:
    static const char* url() {
        return "http://dochub.mongodb.org/core/fsynccommand";
    }

    FSyncCommand() : ErrmsgCommandDeprecated("fsync") {}

    virtual ~FSyncCommand() {
        // The FSyncLockThread is owned by the FSyncCommand and accesses FsyncCommand state. It must
        // be shut down prior to FSyncCommand destruction.
        stdx::unique_lock<Latch> lk(lockStateMutex);
        if (_lockCount > 0) {
            _lockCount = 0;
            releaseFsyncLockSyncCV.notify_one();
            _lockThread->wait();
            _lockThread.reset(nullptr);
        }
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool adminOnly() const {
        return true;
    }
    std::string help() const override {
        return url();
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::fsync);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        if (opCtx->lockState()->isLocked()) {
            errmsg = "fsync: Cannot execute fsync command from contexts that hold a data lock";
            return false;
        }

        const bool sync =
            !cmdObj["async"].trueValue();  // async means do an fsync, but return immediately
        const bool lock = cmdObj["lock"].trueValue();
        log() << "CMD fsync: sync:" << sync << " lock:" << lock;

        // fsync + lock is sometimes used to block writes out of the system and does not care if
        // the `BackupCursorService::fsyncLock` call succeeds.
        const bool allowFsyncFailure =
            getTestCommandsEnabled() && cmdObj["allowFsyncFailure"].trueValue();

        if (!lock) {
            // Take a global IS lock to ensure the storage engine is not shutdown
            Lock::GlobalLock global(opCtx, MODE_IS);
            StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
            result.append("numFiles", storageEngine->flushAllFiles(opCtx, sync));
            return true;
        }

        Lock::ExclusiveLock lk(opCtx->lockState(), commandMutex);
        if (!sync) {
            errmsg = "fsync: sync option must be true when using lock";
            return false;
        }

        const auto lockCountAtStart = getLockCount();
        invariant(lockCountAtStart > 0 || !_lockThread);

        acquireLock();

        if (lockCountAtStart == 0) {

            Status status = Status::OK();
            {
                stdx::unique_lock<Latch> lk(lockStateMutex);
                threadStatus = Status::OK();
                threadStarted = false;
                _lockThread = std::make_unique<FSyncLockThread>(allowFsyncFailure);
                _lockThread->go();

                while (!threadStarted && threadStatus.isOK()) {
                    acquireFsyncLockSyncCV.wait(lk);
                }

                // 'threadStatus' must be copied while 'lockStateMutex' is held.
                status = threadStatus;
            }

            if (!status.isOK()) {
                releaseLock();
                warning() << "fsyncLock failed. Lock count reset to 0. Status: " << status;
                uassertStatusOK(status);
            }
        }

        log() << "mongod is locked and no writes are allowed. db.fsyncUnlock() to unlock";
        log() << "Lock count is " << getLockCount();
        log() << "    For more info see " << FSyncCommand::url();
        result.append("info", "now locked against writes, use db.fsyncUnlock() to unlock");
        result.append("lockCount", getLockCount());
        result.append("seeAlso", FSyncCommand::url());

        return true;
    }

    // Returns whether we are currently fsyncLocked. For use by callers not holding lockStateMutex.
    bool fsyncLocked() {
        stdx::unique_lock<Latch> lkFsyncLocked(_fsyncLockedMutex);
        return _fsyncLocked;
    }

    // For callers not already holding 'lockStateMutex'.
    int64_t getLockCount() {
        stdx::unique_lock<Latch> lk(lockStateMutex);
        return getLockCount_inLock();
    }

    // 'lockStateMutex' must be held when calling.
    int64_t getLockCount_inLock() {
        return _lockCount;
    }

    void releaseLock() {
        stdx::unique_lock<Latch> lk(lockStateMutex);
        releaseLock_inLock(lk);
    }

    void releaseLock_inLock(stdx::unique_lock<Latch>& lk) {
        invariant(_lockCount >= 1);
        _lockCount--;

        if (_lockCount == 0) {
            {
                stdx::unique_lock<Latch> lkFsyncLocked(_fsyncLockedMutex);
                _fsyncLocked = false;
            }
            releaseFsyncLockSyncCV.notify_one();
            lk.unlock();
            _lockThread->wait();
            _lockThread.reset(nullptr);
        }
    }

    // Allows for control of lock state change between the fsyncLock and fsyncUnlock commands and
    // the FSyncLockThread that maintains the global read lock.
    Mutex lockStateMutex = MONGO_MAKE_LATCH("FSyncCommand::lockStateMutex");
    stdx::condition_variable acquireFsyncLockSyncCV;
    stdx::condition_variable releaseFsyncLockSyncCV;

    // 'lockStateMutex' must be held to modify or read.
    Status threadStatus = Status::OK();
    // 'lockStateMutex' must be held to modify or read.
    bool threadStarted = false;

private:
    void acquireLock() {
        stdx::unique_lock<Latch> lk(lockStateMutex);
        _lockCount++;

        if (_lockCount == 1) {
            stdx::unique_lock<Latch> lkFsyncLocked(_fsyncLockedMutex);
            _fsyncLocked = true;
        }
    }

    std::unique_ptr<FSyncLockThread> _lockThread;

    // The number of lock requests currently held. We will only release the fsyncLock when this
    // number is decremented to 0. May only be accessed while 'lockStateMutex' is held.
    int64_t _lockCount = 0;

    Mutex _fsyncLockedMutex = MONGO_MAKE_LATCH("FSyncCommand::_fsyncLockedMutex");
    bool _fsyncLocked = false;
} fsyncCmd;

class FSyncUnlockCommand : public ErrmsgCommandDeprecated {
public:
    FSyncUnlockCommand() : ErrmsgCommandDeprecated("fsyncUnlock") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::unlock);

        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& db,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        log() << "command: unlock requested";

        Lock::ExclusiveLock lk(opCtx->lockState(), commandMutex);

        stdx::unique_lock<Latch> stateLock(fsyncCmd.lockStateMutex);

        auto lockCount = fsyncCmd.getLockCount_inLock();
        if (lockCount == 0) {
            errmsg = "fsyncUnlock called when not locked";
            return false;
        }

        fsyncCmd.releaseLock_inLock(stateLock);

        // Relies on the lock to be released in 'releaseLock_inLock()' when the release brings
        // the lock count to 0.
        if (stateLock) {
            // If we're still locked then lock count is not zero.
            invariant(lockCount > 0);
            lockCount = fsyncCmd.getLockCount_inLock();
            log() << "fsyncUnlock completed. Lock count is now " << lockCount;
        } else {
            invariant(fsyncCmd.getLockCount() == 0);
            lockCount = 0;
            log() << "fsyncUnlock completed. mongod is now unlocked and free to accept writes";
        }

        result.append("info", str::stream() << "fsyncUnlock completed");
        result.append("lockCount", lockCount);
        return true;
    }

} unlockFsyncCmd;

// Exposed publically via extern in fsync.h.
SimpleMutex filesLockedFsync;

bool FSyncLockThread::_shutdownTaskRegistered = false;

void FSyncLockThread::run() {
    ThreadClient tc("fsyncLockWorker", getGlobalServiceContext());
    stdx::lock_guard<SimpleMutex> lkf(filesLockedFsync);
    stdx::unique_lock<Latch> lk(fsyncCmd.lockStateMutex);

    invariant(fsyncCmd.getLockCount_inLock() == 1);

    try {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::GlobalRead global(&opCtx);  // Block any writes in order to flush the files.

        ServiceContext* serviceContext = opCtx.getServiceContext();
        StorageEngine* storageEngine = serviceContext->getStorageEngine();

        // The fsync shutdown task has to be registered once the server is running otherwise it
        // conflicts with the servers shutdown task.
        if (!_shutdownTaskRegistered) {
            _shutdownTaskRegistered = true;
            registerShutdownTask([&] {
                stdx::unique_lock<Latch> stateLock(fsyncCmd.lockStateMutex);
                if (fsyncCmd.getLockCount_inLock() > 0) {
                    warning() << "Interrupting fsync because the server is shutting down.";
                    while (fsyncCmd.getLockCount_inLock()) {
                        // Relies on the lock to be released in 'releaseLock_inLock()' when the
                        // release brings the lock count to 0.
                        invariant(stateLock);
                        fsyncCmd.releaseLock_inLock(stateLock);
                    }
                }
            });
        }

        try {
            storageEngine->flushAllFiles(&opCtx, true);
        } catch (const std::exception& e) {
            error() << "error doing flushAll: " << e.what();
            fsyncCmd.threadStatus = Status(ErrorCodes::CommandFailed, e.what());
            fsyncCmd.acquireFsyncLockSyncCV.notify_one();
            return;
        }

        bool successfulFsyncLock = false;
        auto backupCursorHooks = BackupCursorHooks::get(serviceContext);
        try {
            writeConflictRetry(&opCtx,
                               "beginBackup",
                               "global",
                               [&opCtx, backupCursorHooks, &successfulFsyncLock, storageEngine] {
                                   if (backupCursorHooks->enabled()) {
                                       backupCursorHooks->fsyncLock(&opCtx);
                                       successfulFsyncLock = true;
                                   } else {
                                       // Have the uassert be caught by the DBException
                                       // block. Maintain "allowFsyncFailure" compatibility in
                                       // community.
                                       uassertStatusOK(storageEngine->beginBackup(&opCtx));
                                       successfulFsyncLock = true;
                                   }
                               });
        } catch (const DBException& e) {
            if (_allowFsyncFailure) {
                warning() << "Locking despite storage engine being unable to begin backup : "
                          << e.toString();
            } else {
                error() << "storage engine unable to begin backup : " << e.toString();
                fsyncCmd.threadStatus = e.toStatus();
                fsyncCmd.acquireFsyncLockSyncCV.notify_one();
                return;
            }
        }

        fsyncCmd.threadStarted = true;
        fsyncCmd.acquireFsyncLockSyncCV.notify_one();

        while (fsyncCmd.getLockCount_inLock() > 0) {
            warning() << "WARNING: instance is locked, blocking all writes. The fsync command has "
                         "finished execution, remember to unlock the instance using fsyncUnlock().";
            fsyncCmd.releaseFsyncLockSyncCV.wait_for(lk, Seconds(60).toSystemDuration());
        }

        if (successfulFsyncLock) {
            if (backupCursorHooks->enabled()) {
                backupCursorHooks->fsyncUnlock(&opCtx);
            } else {
                storageEngine->endBackup(&opCtx);
            }
        }

    } catch (const std::exception& e) {
        severe() << "FSyncLockThread exception: " << e.what();
        fassertFailed(40350);
    }
}

MONGO_INITIALIZER(fsyncLockedForWriting)(InitializerContext* context) {
    setLockedForWritingImpl([]() { return fsyncCmd.fsyncLocked(); });
    return Status::OK();
}
}  // namespace mongo
