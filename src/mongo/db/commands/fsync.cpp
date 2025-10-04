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


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/commands/fsync.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync_gen.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <exception>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

// Protects access to globalFsyncLockThread and other global fsync state.
stdx::mutex fsyncStateMutex;

// Globally accessible FsyncLockThread to allow shutdown to coordinate with any active fsync cmds.
// Must acquire the 'fsyncStateMutex' before accessing.
std::unique_ptr<FSyncLockThread> globalFsyncLockThread = nullptr;

// Exposed publically via extern in fsync.h.
stdx::mutex oplogWriterLockedFsync;
stdx::mutex oplogApplierLockedFsync;

namespace {

// Ensures that only one command is operating on fsyncLock state at a time. As a 'ResourceMutex',
// lock time will be reported for a given user operation.
Lock::ResourceMutex fsyncSingleCommandExclusionMutex("fsyncSingleCommandExclusionMutex");

class FSyncCore {
public:
    static const char* url() {
        return "http://dochub.mongodb.org/core/fsynccommand";
    }

    ~FSyncCore() {
        // The FSyncLockThread is owned by the FSyncCommand and accesses FsyncCommand state. It must
        // be shut down prior to FSyncCommand destruction.
        stdx::unique_lock<stdx::mutex> lk(fsyncStateMutex);
        if (_lockCount > 0) {
            _lockCount = 0;
            releaseFsyncLockSyncCV.notify_one();
            globalFsyncLockThread->wait();
            globalFsyncLockThread.reset(nullptr);
        }
    }

    void checkForInProgressDDLOperations(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        const auto numDDLDocuments =
            client.count(NamespaceString::kShardingDDLCoordinatorsNamespace);

        if (numDDLDocuments != 0) {
            LOGV2_WARNING(781541, "Cannot take lock while DDL operations is in progress");
            releaseLock();
            uasserted(ErrorCodes::IllegalOperation,
                      "Cannot take lock while DDL operation is in progress");
        }
    }

    void runFsyncCommand(OperationContext* opCtx,
                         const FSyncRequest& request,
                         BSONObjBuilder& result) {
        uassert(ErrorCodes::IllegalOperation,
                "fsync: Cannot execute fsync command from contexts that hold a data lock",
                !shard_role_details::getLocker(opCtx)->isLocked());

        const bool lock = request.getLock();
        const bool forBackup = request.getForBackup();
        LOGV2(20461, "CMD fsync", "lock"_attr = lock, "forBackup"_attr = forBackup);

        // fsync + lock is sometimes used to block writes out of the system and does not care if
        // the `BackupCursorService::fsyncLock` call succeeds.
        const bool allowFsyncFailure = getTestCommandsEnabled() && request.getAllowFsyncFailure();

        if (!lock) {
            // Take a global IS lock to ensure the storage engine is not shutdown
            auto* const storageEngine = opCtx->getServiceContext()->getStorageEngine();
            Lock::GlobalLock global(opCtx, MODE_IS);
            storageEngine->flushAllFiles(opCtx, /*callerHoldsReadLock*/ true);

            // This field has had a dummy value since MMAP went away. It is undocumented.
            // Maintaining it so as not to cause unnecessary user pain across upgrades.
            result.append("numFiles", 1);
            return;
        }

        Lock::ExclusiveLock lk(opCtx, fsyncSingleCommandExclusionMutex);

        const auto lockCountAtStart = getLockCount();
        invariant(lockCountAtStart > 0 || !globalFsyncLockThread);

        acquireLock();

        if (lockCountAtStart == 0) {
            Status status = Status::OK();
            {
                stdx::unique_lock<stdx::mutex> lk(fsyncStateMutex);
                threadStatus = Status::OK();
                threadStarted = false;
                Milliseconds deadline = Milliseconds::max();
                if (forBackup) {
                    // Defaults to 90s (90,000 ms), see fsync.idl.
                    deadline = Milliseconds{request.getFsyncLockAcquisitionTimeoutMillis()};
                }
                globalFsyncLockThread = std::make_unique<FSyncLockThread>(
                    opCtx->getServiceContext(), allowFsyncFailure, deadline);
                globalFsyncLockThread->go();

                while (!threadStarted && threadStatus.isOK()) {
                    acquireFsyncLockSyncCV.wait(lk);
                }

                // 'threadStatus' must be copied while 'fsyncStateMutex' is held.
                status = threadStatus;
            }

            if (!status.isOK()) {
                releaseLock();
                LOGV2_WARNING(20468,
                              "fsyncLock failed. Lock count reset to 0. Status: {error}",
                              "error"_attr = status);
                uassertStatusOK(status);
            }
        }
        if (forBackup) {
            // The check must be performed only if the fsync+lock command has been issued for backup
            // purposes (through monogs). There are valid cases where fsync+lock can be invoked on
            // the mongod while DDLs are in progress.
            checkForInProgressDDLOperations(opCtx);
        }

        LOGV2(20462,
              "mongod is locked and no writes are allowed",
              "lockCount"_attr = getLockCount(),
              "seeAlso"_attr = url());
        result.append("info", "now locked against writes, use db.fsyncUnlock() to unlock");
        result.append("lockCount", getLockCount());
        result.append("seeAlso", url());

        return;
    }

    bool runFsyncUnlockCommand(OperationContext* opCtx,
                               const BSONObj& cmdObj,
                               BSONObjBuilder& result);

    /**
     * Returns whether we are currently fsyncLocked. For use by callers not holding fsyncStateMutex.
     */
    bool fsyncLocked() {
        stdx::unique_lock<stdx::mutex> lkFsyncLocked(_fsyncLockedMutex);
        return _fsyncLocked;
    }

    /**
     * For callers not already holding 'fsyncStateMutex'.
     */
    int64_t getLockCount() {
        stdx::unique_lock<stdx::mutex> lk(fsyncStateMutex);
        return getLockCount_inLock();
    }

    /**
     * 'fsyncStateMutex' must be held when calling.
     */
    int64_t getLockCount_inLock() {
        return _lockCount;
    }

    void releaseLock() {
        stdx::unique_lock<stdx::mutex> lk(fsyncStateMutex);
        releaseLock_inLock(lk);
    }

    /**
     * Returns false if the fsync lock was recursively locked. Returns true if the fysnc lock is
     * released.
     */
    bool releaseLock_inLock(stdx::unique_lock<stdx::mutex>& lk) {
        invariant(_lockCount >= 1);
        _lockCount--;

        if (_lockCount == 0) {
            {
                stdx::unique_lock<stdx::mutex> lkFsyncLocked(_fsyncLockedMutex);
                _fsyncLocked = false;
            }
            releaseFsyncLockSyncCV.notify_one();
            lk.unlock();
            globalFsyncLockThread->wait();
            globalFsyncLockThread.reset(nullptr);
            return true;
        }
        return false;
    }

    // Allows for control of lock state change between the fsyncLock and fsyncUnlock commands and
    // the FSyncLockThread that maintains the global read lock.
    stdx::condition_variable acquireFsyncLockSyncCV;
    stdx::condition_variable releaseFsyncLockSyncCV;

    // 'fsyncStateMutex' must be held to modify or read.
    Status threadStatus = Status::OK();
    // 'fsyncStateMutex' must be held to modify or read.
    bool threadStarted = false;

private:
    void acquireLock() {
        stdx::unique_lock<stdx::mutex> lk(fsyncStateMutex);
        _lockCount++;

        if (_lockCount == 1) {
            stdx::unique_lock<stdx::mutex> lkFsyncLocked(_fsyncLockedMutex);
            _fsyncLocked = true;
        }
    }

    // The number of lock requests currently held. We will only release the fsyncLock when this
    // number is decremented to 0. May only be accessed while 'fsyncStateMutex' is held.
    int64_t _lockCount = 0;

    stdx::mutex _fsyncLockedMutex;
    bool _fsyncLocked = false;
};
FSyncCore fsyncCore;

class FSyncCommand : public TypedCommand<FSyncCommand> {
public:
    using Request = FSyncRequest;

    /**
     * Intermediate wrapper to interface with ReplyBuilderInterface.
     */
    class Response {
    public:
        explicit Response(BSONObj obj) : _obj(std::move(obj)) {}

        void serialize(BSONObjBuilder* builder) const {
            builder->appendElements(_obj);
        }

    private:
        const BSONObj _obj;
    };

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            BSONObjBuilder result;
            fsyncCore.runFsyncCommand(opCtx, this->request(), result);
            return Response{result.obj()};
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return {};
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(
                ErrorCodes::Unauthorized,
                "unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(as->getUserTenantId()), ActionType::fsync));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return FSyncCore::url();
    }
};
MONGO_REGISTER_COMMAND(FSyncCommand).forShard();

class FSyncUnlockCommand : public BasicCommand {
public:
    FSyncUnlockCommand() : BasicCommand("fsyncUnlock") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        bool isAuthorized =
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::unlock);

        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        return fsyncCore.runFsyncUnlockCommand(opCtx, cmdObj, result);
    }
};
MONGO_REGISTER_COMMAND(FSyncUnlockCommand).forShard();

bool FSyncCore::runFsyncUnlockCommand(OperationContext* opCtx,
                                      const BSONObj& cmdObj,
                                      BSONObjBuilder& result) {
    {
        LOGV2(20465, "command: unlock requested");

        Lock::ExclusiveLock lk(opCtx, fsyncSingleCommandExclusionMutex);

        stdx::unique_lock<stdx::mutex> stateLock(fsyncStateMutex);

        auto lockCount = getLockCount_inLock();

        uassert(ErrorCodes::IllegalOperation, "fsyncUnlock called when not locked", lockCount != 0);

        releaseLock_inLock(stateLock);

        // Relies on the lock to be released in 'releaseLock_inLock()' when the release brings
        // the lock count to 0.
        if (stateLock) {
            // If we're still locked then lock count is not zero.
            invariant(lockCount > 0);
            lockCount = getLockCount_inLock();
        } else {
            invariant(getLockCount() == 0);
            lockCount = 0;
        }

        LOGV2(20466, "fsyncUnlock complete", "lockCount"_attr = lockCount);

        result.append("info", str::stream() << "fsyncUnlock completed");
        result.append("lockCount", lockCount);
        return true;
    }
}

}  // namespace

void FSyncLockThread::shutdown(stdx::unique_lock<stdx::mutex>& stateLock) {
    if (fsyncCore.getLockCount_inLock() > 0) {
        LOGV2_WARNING(20469, "Interrupting fsync because the server is shutting down");
        while (!fsyncCore.releaseLock_inLock(stateLock))
            ;
    }
}

void FSyncLockThread::_waitUntilLastAppliedCatchupLastWritten() {
    auto replCoord = repl::ReplicationCoordinator::get(_serviceContext);
    while (replCoord->getMyLastAppliedOpTimeAndWallTime() !=
           replCoord->getMyLastWrittenOpTimeAndWallTime()) {
        sleepmillis(100);
    }
}

void FSyncLockThread::run() {
    ThreadClient tc("fsyncLockWorker", _serviceContext->getService(ClusterRole::ShardServer));
    // We first lock to stop oplogWriter then wait oplogApplier to apply everything in its buffer.
    // This can make sure we have applied all oplogs that have been written when we fsync lock the
    // server.
    stdx::lock_guard<stdx::mutex> writerLk(oplogWriterLockedFsync);
    _waitUntilLastAppliedCatchupLastWritten();
    stdx::lock_guard<stdx::mutex> applierLk(oplogApplierLockedFsync);
    stdx::unique_lock<stdx::mutex> stateLock(fsyncStateMutex);

    invariant(fsyncCore.getLockCount_inLock() == 1);

    try {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;

        // If the deadline exists, set it on the opCtx and GlobalRead lock.
        Date_t lockDeadline = Date_t::max();
        if (_deadline < Milliseconds::max()) {
            lockDeadline = Date_t::now() + _deadline;
        }

        opCtx.setDeadlineAfterNowBy(Milliseconds(_deadline), ErrorCodes::ExceededTimeLimit);
        Lock::GlobalRead global(&opCtx, lockDeadline, Lock::InterruptBehavior::kThrow);
        StorageEngine* storageEngine = _serviceContext->getStorageEngine();

        try {
            storageEngine->flushAllFiles(&opCtx, /*callerHoldsReadLock*/ true);
        } catch (const std::exception& e) {
            if (!_allowFsyncFailure) {
                LOGV2_ERROR(20472, "Error doing flushAll", "error"_attr = e.what());
                fsyncCore.threadStatus = Status(ErrorCodes::CommandFailed, e.what());
                fsyncCore.acquireFsyncLockSyncCV.notify_one();
                return;
            }
        }

        bool successfulFsyncLock = false;
        auto backupCursorHooks = BackupCursorHooks::get(_serviceContext);
        try {
            writeConflictRetry(&opCtx,
                               "beginBackup",
                               NamespaceString(DatabaseName::kGlobal),
                               [&opCtx, backupCursorHooks, &successfulFsyncLock, storageEngine] {
                                   if (backupCursorHooks->enabled()) {
                                       backupCursorHooks->fsyncLock(&opCtx);
                                       successfulFsyncLock = true;
                                   } else {
                                       // Have the uassert be caught by the DBException
                                       // block. Maintain "allowFsyncFailure" compatibility in
                                       // community.
                                       uassertStatusOK(storageEngine->beginBackup());
                                       successfulFsyncLock = true;
                                   }
                               });
        } catch (const DBException& e) {
            if (_allowFsyncFailure) {
                LOGV2_WARNING(20470,
                              "Locking despite storage engine being unable to begin backup",
                              "error"_attr = e);
            } else {
                LOGV2_ERROR(20473, "Storage engine unable to begin backup", "error"_attr = e);
                fsyncCore.threadStatus = e.toStatus();
                fsyncCore.acquireFsyncLockSyncCV.notify_one();
                return;
            }
        }

        fsyncCore.threadStarted = true;
        fsyncCore.acquireFsyncLockSyncCV.notify_one();

        while (fsyncCore.getLockCount_inLock() > 0) {
            LOGV2_WARNING(
                20471,
                "WARNING: instance is locked, blocking all writes. The fsync command has "
                "finished execution, remember to unlock the instance using fsyncUnlock().");
            fsyncCore.releaseFsyncLockSyncCV.wait_for(stateLock, Seconds(60).toSystemDuration());
        }

        if (successfulFsyncLock) {
            if (backupCursorHooks->enabled()) {
                backupCursorHooks->fsyncUnlock(&opCtx);
            } else {
                storageEngine->endBackup();
            }
        }

    } catch (const ExceptionFor<ErrorCategory::ExceededTimeLimitError>&) {
        LOGV2_ERROR(204739, "Fsync timed out with ExceededTimeLimitError");
        fsyncCore.threadStatus = Status(ErrorCodes::Error::LockTimeout, "Fsync lock timed out");
        fsyncCore.acquireFsyncLockSyncCV.notify_one();
        return;
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        LOGV2_ERROR(204740, "Fsync timed out with LockTimeout");
        fsyncCore.threadStatus = Status(ErrorCodes::Error::LockTimeout, "Fsync lock timed out");
        fsyncCore.acquireFsyncLockSyncCV.notify_one();
        return;
    } catch (ExceptionFor<ErrorCategory::Interruption>& ex) {
        LOGV2_ERROR(204741, "Fsync interrupted", "{reason}"_attr = ex.reason());
        fsyncCore.threadStatus = ex.toStatus();
        fsyncCore.acquireFsyncLockSyncCV.notify_one();
        return;
    } catch (const std::exception& e) {
        LOGV2_FATAL(40350, "FSyncLockThread exception", "error"_attr = e.what());
    }
}

MONGO_INITIALIZER(fsyncLockedForWriting)(InitializerContext* context) {
    setLockedForWritingImpl([]() { return fsyncCore.fsyncLocked(); });
}

}  // namespace mongo
