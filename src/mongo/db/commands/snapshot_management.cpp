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

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {
class CmdMakeSnapshot final : public BasicCommand {
public:
    CmdMakeSnapshot() : BasicCommand("makeSnapshot") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    bool adminOnly() const override {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    std::string help() const override {
        return "Creates a new named snapshot";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        uassert(ErrorCodes::CommandNotSupported,
                "makeSnapshot requires a persistent storage engine and a snapshot manager",
                !storageEngine->isEphemeral() && storageEngine->getSnapshotManager());

        Lock::GlobalLock lk(opCtx, MODE_IX);

        const auto latestTs = repl::StorageInterface::get(opCtx)->getLatestOplogTimestamp(opCtx);
        result.append("name", static_cast<long long>(latestTs.asULL()));

        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdMakeSnapshot).testOnly().forShard();

class CmdSetCommittedSnapshot final : public BasicCommand {
public:
    CmdSetCommittedSnapshot() : BasicCommand("setCommittedSnapshot") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    bool adminOnly() const override {
        return true;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    std::string help() const override {
        return "Sets the snapshot for {readConcern: {level: 'majority'}}";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto snapshotManager = getGlobalServiceContext()->getStorageEngine()->getSnapshotManager();
        if (!snapshotManager) {
            uasserted(ErrorCodes::CommandNotSupported, "");
        }

        Lock::GlobalLock lk(opCtx, MODE_IX);
        auto timestamp = Timestamp(cmdObj.firstElement().Long());
        snapshotManager->setCommittedSnapshot(timestamp);
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdSetCommittedSnapshot).testOnly().forShard();
}  // namespace mongo
