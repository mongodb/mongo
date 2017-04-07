/**
 *    Copyright (C) 2012 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/commands/fsync.h"

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace {
// Ensures that only one command is operating on fsyncLock state at a time. As a 'ResourceMutex',
// lock time will be reported for a given user operation.
Lock::ResourceMutex commandMutex("fsyncCommandMutex");
}

/**
 * Maintains a global read lock while mongod is fsyncLocked.
 */
class FSyncLockThread : public BackgroundJob {
public:
    FSyncLockThread() : BackgroundJob(false) {}
    virtual ~FSyncLockThread() {}
    virtual string name() const {
        return "FSyncLockThread";
    }
    virtual void run();
};

class FSyncCommand : public Command {
public:
    static const char* url() {
        return "http://dochub.mongodb.org/core/fsynccommand";
    }

    FSyncCommand() : Command("fsync") {}

    virtual ~FSyncCommand() {
        // The FSyncLockThread is owned by the FSyncCommand and accesses FsyncCommand state. It must
        // be shut down prior to FSyncCommand destruction.
        stdx::unique_lock<stdx::mutex> lk(lockStateMutex);
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
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual void help(stringstream& h) const {
        h << url();
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::fsync);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     BSONObj& cmdObj,
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

        if (!lock) {
            // the simple fsync command case
            if (sync) {
                // can this be GlobalRead? and if it can, it should be nongreedy.
                Lock::GlobalWrite w(opCtx);
                // TODO SERVER-26822: Replace MMAPv1 specific calls with ones that are storage
                // engine agnostic.
                getDur().commitNow(opCtx);

                //  No WriteUnitOfWork needed, as this does no writes of its own.
            }

            // Take a global IS lock to ensure the storage engine is not shutdown
            Lock::GlobalLock global(opCtx, MODE_IS, UINT_MAX);
            StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
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
                stdx::unique_lock<stdx::mutex> lk(lockStateMutex);
                threadStatus = Status::OK();
                threadStarted = false;
                _lockThread = stdx::make_unique<FSyncLockThread>();
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
                return appendCommandStatus(result, status);
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
        stdx::unique_lock<stdx::mutex> lkFsyncLocked(_fsyncLockedMutex);
        return _fsyncLocked;
    }

    // For callers not already holding 'lockStateMutex'.
    int64_t getLockCount() {
        stdx::unique_lock<stdx::mutex> lk(lockStateMutex);
        return getLockCount_inLock();
    }

    // 'lockStateMutex' must be held when calling.
    int64_t getLockCount_inLock() {
        return _lockCount;
    }

    void releaseLock() {
        stdx::unique_lock<stdx::mutex> lk(lockStateMutex);
        invariant(_lockCount >= 1);
        _lockCount--;

        if (_lockCount == 0) {
            {
                stdx::unique_lock<stdx::mutex> lkFsyncLocked(_fsyncLockedMutex);
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
    stdx::mutex lockStateMutex;
    stdx::condition_variable acquireFsyncLockSyncCV;
    stdx::condition_variable releaseFsyncLockSyncCV;

    // 'lockStateMutex' must be held to modify or read.
    Status threadStatus = Status::OK();
    // 'lockStateMutex' must be held to modify or read.
    bool threadStarted = false;

private:
    void acquireLock() {
        stdx::unique_lock<stdx::mutex> lk(lockStateMutex);
        _lockCount++;

        if (_lockCount == 1) {
            stdx::unique_lock<stdx::mutex> lkFsyncLocked(_fsyncLockedMutex);
            _fsyncLocked = true;
        }
    }

    std::unique_ptr<FSyncLockThread> _lockThread;

    // The number of lock requests currently held. We will only release the fsyncLock when this
    // number is decremented to 0. May only be accessed while 'lockStateMutex' is held.
    int64_t _lockCount = 0;

    stdx::mutex _fsyncLockedMutex;
    bool _fsyncLocked = false;
} fsyncCmd;

class FSyncUnlockCommand : public Command {
public:
    FSyncUnlockCommand() : Command("fsyncUnlock") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::unlock);

        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* opCtx,
             const std::string& db,
             BSONObj& cmdObj,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        log() << "command: unlock requested";

        Lock::ExclusiveLock lk(opCtx->lockState(), commandMutex);

        if (unlockFsync()) {
            const auto lockCount = fsyncCmd.getLockCount();
            result.append("info", str::stream() << "fsyncUnlock completed");
            result.append("lockCount", lockCount);
            if (lockCount == 0) {
                log() << "fsyncUnlock completed. mongod is now unlocked and free to accept writes";
            } else {
                log() << "fsyncUnlock completed. Lock count is now " << lockCount;
            }
            return true;
        } else {
            errmsg = "fsyncUnlock called when not locked";
            return false;
        }
    }

private:
    // Returns true if lock count is decremented.
    bool unlockFsync() {
        if (fsyncCmd.getLockCount() == 0) {
            error() << "fsyncUnlock called when not locked";
            return false;
        }

        fsyncCmd.releaseLock();
        return true;
    }

} unlockFsyncCmd;

// Exposed publically via extern in fsync.h.
SimpleMutex filesLockedFsync;

void FSyncLockThread::run() {
    Client::initThread("fsyncLockWorker");
    stdx::lock_guard<SimpleMutex> lkf(filesLockedFsync);
    stdx::unique_lock<stdx::mutex> lk(fsyncCmd.lockStateMutex);

    invariant(fsyncCmd.getLockCount_inLock() == 1);

    try {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        Lock::GlobalWrite global(&opCtx);  // No WriteUnitOfWork needed

        try {
            // TODO SERVER-26822: Replace MMAPv1 specific calls with ones that are storage engine
            // agnostic.
            getDur().syncDataAndTruncateJournal(&opCtx);
        } catch (const std::exception& e) {
            error() << "error doing syncDataAndTruncateJournal: " << e.what();
            fsyncCmd.threadStatus = Status(ErrorCodes::CommandFailed, e.what());
            fsyncCmd.acquireFsyncLockSyncCV.notify_one();
            return;
        }
        opCtx.lockState()->downgradeGlobalXtoSForMMAPV1();
        StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();

        try {
            storageEngine->flushAllFiles(&opCtx, true);
        } catch (const std::exception& e) {
            error() << "error doing flushAll: " << e.what();
            fsyncCmd.threadStatus = Status(ErrorCodes::CommandFailed, e.what());
            fsyncCmd.acquireFsyncLockSyncCV.notify_one();
            return;
        }
        try {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                uassertStatusOK(storageEngine->beginBackup(&opCtx));
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(&opCtx, "beginBackup", "global");
        } catch (const DBException& e) {
            error() << "storage engine unable to begin backup : " << e.toString();
            fsyncCmd.threadStatus = e.toStatus();
            fsyncCmd.acquireFsyncLockSyncCV.notify_one();
            return;
        }

        fsyncCmd.threadStarted = true;
        fsyncCmd.acquireFsyncLockSyncCV.notify_one();

        while (fsyncCmd.getLockCount_inLock() > 0) {
            fsyncCmd.releaseFsyncLockSyncCV.wait(lk);
        }

        storageEngine->endBackup(&opCtx);

    } catch (const std::exception& e) {
        severe() << "FSyncLockThread exception: " << e.what();
        fassertFailed(40350);
    }
}

bool lockedForWriting() {
    return fsyncCmd.fsyncLocked();
}
}
