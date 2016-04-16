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

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/audit.h"
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

using std::endl;
using std::string;
using std::stringstream;

class FSyncLockThread : public BackgroundJob {
    void doRealWork();

public:
    FSyncLockThread() : BackgroundJob(true) {}
    virtual ~FSyncLockThread() {}
    virtual string name() const {
        return "FSyncLockThread";
    }
    virtual void run() {
        Client::initThread("fsyncLockWorker");
        try {
            doRealWork();
        } catch (std::exception& e) {
            error() << "FSyncLockThread exception: " << e.what() << endl;
        }
    }
};

/* see unlockFsync() for unlocking:
   db.$cmd.sys.unlock.findOne()
*/
class FSyncCommand : public Command {
public:
    static const char* url() {
        return "http://dochub.mongodb.org/core/fsynccommand";
    }
    bool locked;
    bool pendingUnlock;
    SimpleMutex m;  // protects locked var above
    Status status = Status::OK();

    stdx::condition_variable_any _threadSync;
    stdx::condition_variable_any _unlockSync;

    FSyncCommand() : Command("fsync") {
        locked = false;
        pendingUnlock = false;
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
    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        if (txn->lockState()->isLocked()) {
            errmsg = "fsync: Cannot execute fsync command from contexts that hold a data lock";
            return false;
        }

        bool sync =
            !cmdObj["async"].trueValue();  // async means do an fsync, but return immediately
        bool lock = cmdObj["lock"].trueValue();
        log() << "CMD fsync: sync:" << sync << " lock:" << lock << endl;
        if (lock) {
            if (!sync) {
                errmsg = "fsync: sync option must be true when using lock";
                return false;
            }

            stdx::lock_guard<SimpleMutex> lk(m);
            status = Status::OK();

            (new FSyncLockThread())->go();
            while (!locked && status.isOK()) {
                _threadSync.wait(m);
            }

            if (!status.isOK())
                return appendCommandStatus(result, status);

            log() << "db is now locked, no writes allowed. db.fsyncUnlock() to unlock" << endl;
            log() << "    For more info see " << FSyncCommand::url() << endl;
            result.append("info", "now locked against writes, use db.fsyncUnlock() to unlock");
            result.append("seeAlso", FSyncCommand::url());

        } else {
            // the simple fsync command case
            if (sync) {
                // can this be GlobalRead? and if it can, it should be nongreedy.
                ScopedTransaction transaction(txn, MODE_X);
                Lock::GlobalWrite w(txn->lockState());
                getDur().commitNow(txn);

                //  No WriteUnitOfWork needed, as this does no writes of its own.
            }

            // Take a global IS lock to ensure the storage engine is not shutdown
            Lock::GlobalLock global(txn->lockState(), MODE_IS, UINT_MAX);
            StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
            result.append("numFiles", storageEngine->flushAllFiles(sync));
        }
        return 1;
    }
} fsyncCmd;

namespace {
bool unlockFsync();
}  // namespace

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

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::unlock);

        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        log() << "command: unlock requested";

        if (unlockFsync()) {
            result.append("info", "unlock completed");
            return true;
        } else {
            errmsg = "not locked";
            return false;
        }
    }

} unlockFsyncCmd;

SimpleMutex filesLockedFsync;

void FSyncLockThread::doRealWork() {
    stdx::lock_guard<SimpleMutex> lkf(filesLockedFsync);

    const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
    OperationContext& txn = *txnPtr;
    ScopedTransaction transaction(&txn, MODE_X);
    Lock::GlobalWrite global(txn.lockState());  // No WriteUnitOfWork needed

    stdx::lock_guard<SimpleMutex> lk(fsyncCmd.m);

    invariant(!fsyncCmd.locked);  // impossible to get here if locked is true
    try {
        getDur().syncDataAndTruncateJournal(&txn);
    } catch (std::exception& e) {
        error() << "error doing syncDataAndTruncateJournal: " << e.what() << endl;
        fsyncCmd.status = Status(ErrorCodes::CommandFailed, e.what());
        fsyncCmd._threadSync.notify_one();
        fsyncCmd.locked = false;
        return;
    }
    txn.lockState()->downgradeGlobalXtoSForMMAPV1();
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();

    try {
        storageEngine->flushAllFiles(true);
    } catch (std::exception& e) {
        error() << "error doing flushAll: " << e.what() << endl;
        fsyncCmd.status = Status(ErrorCodes::CommandFailed, e.what());
        fsyncCmd._threadSync.notify_one();
        fsyncCmd.locked = false;
        return;
    }
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            uassertStatusOK(storageEngine->beginBackup(&txn));
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(&txn, "beginBackup", "global");
    } catch (const DBException& e) {
        error() << "storage engine unable to begin backup : " << e.toString() << endl;
        fsyncCmd.status = e.toStatus();
        fsyncCmd._threadSync.notify_one();
        fsyncCmd.locked = false;
        return;
    }

    invariant(!fsyncCmd.locked);
    fsyncCmd.locked = true;

    fsyncCmd._threadSync.notify_one();

    while (!fsyncCmd.pendingUnlock) {
        fsyncCmd._unlockSync.wait(fsyncCmd.m);
    }

    storageEngine->endBackup(&txn);

    fsyncCmd.pendingUnlock = false;

    fsyncCmd.locked = false;
    fsyncCmd.status = Status::OK();

    fsyncCmd._unlockSync.notify_one();
}

bool lockedForWriting() {
    return fsyncCmd.locked;
}

namespace {
// @return true if unlocked
bool unlockFsync() {
    stdx::lock_guard<SimpleMutex> lk(fsyncCmd.m);
    if (!fsyncCmd.locked) {
        return false;
    }
    fsyncCmd.pendingUnlock = true;
    fsyncCmd._unlockSync.notify_one();
    fsyncCmd._threadSync.notify_one();

    while (fsyncCmd.locked) {
        fsyncCmd._unlockSync.wait(fsyncCmd.m);
    }
    return true;
}
}  // namespace
}
