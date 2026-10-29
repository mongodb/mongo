// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
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

    bool requiresAuthzChecks() const override {
        return false;
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
