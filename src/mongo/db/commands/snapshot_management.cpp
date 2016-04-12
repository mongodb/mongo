/*
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot_manager.h"

namespace mongo {
class CmdMakeSnapshot final : public Command {
public:
    CmdMakeSnapshot() : Command("makeSnapshot") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return Status::OK();
    }

    virtual void help(std::stringstream& h) const {
        h << "Creates a new named snapshot";
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int,
             std::string& errmsg,
             BSONObjBuilder& result) {
        auto snapshotManager =
            getGlobalServiceContext()->getGlobalStorageEngine()->getSnapshotManager();
        if (!snapshotManager) {
            return appendCommandStatus(result, {ErrorCodes::CommandNotSupported, ""});
        }

        ScopedTransaction st(txn, MODE_IX);
        Lock::GlobalLock lk(txn->lockState(), MODE_IX, UINT_MAX);

        auto status = snapshotManager->prepareForCreateSnapshot(txn);
        if (status.isOK()) {
            const auto name = repl::ReplicationCoordinator::get(txn)->reserveSnapshotName(nullptr);
            result.append("name", static_cast<long long>(name.asU64()));
            status = snapshotManager->createSnapshot(txn, name);
        }
        return appendCommandStatus(result, status);
    }
};

class CmdSetCommittedSnapshot final : public Command {
public:
    CmdSetCommittedSnapshot() : Command("setCommittedSnapshot") {}

    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return Status::OK();
    }

    virtual void help(std::stringstream& h) const {
        h << "Sets the snapshot for {readConcern: {level: 'majority'}}";
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int,
             std::string& errmsg,
             BSONObjBuilder& result) {
        auto snapshotManager =
            getGlobalServiceContext()->getGlobalStorageEngine()->getSnapshotManager();
        if (!snapshotManager) {
            return appendCommandStatus(result, {ErrorCodes::CommandNotSupported, ""});
        }

        ScopedTransaction st(txn, MODE_IX);
        Lock::GlobalLock lk(txn->lockState(), MODE_IX, UINT_MAX);
        auto name = SnapshotName(cmdObj.firstElement().Long());
        snapshotManager->setCommittedSnapshot(name);
        return true;
    }
};

MONGO_INITIALIZER(RegisterSnapshotManagementCommands)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        // Leaked intentionally: a Command registers itself when constructed.
        new CmdMakeSnapshot();
        new CmdSetCommittedSnapshot();
    }
    return Status::OK();
}
}
