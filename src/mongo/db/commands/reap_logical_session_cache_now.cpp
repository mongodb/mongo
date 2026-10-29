// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"

#include <string>

namespace mongo {
namespace {

class ReapLogicalSessionCacheNowCommand final : public BasicCommand {
public:
    ReapLogicalSessionCacheNowCommand() : BasicCommand("reapLogicalSessionCacheNow") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "force the logical session cache to reap. Test command only.";
    }

    // No auth needed because it only works when enabled via command line.
    // See docs/test_commands.md.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto cache = LogicalSessionCache::get(opCtx);

        cache->reapNow(opCtx);
        return true;
    }
};

MONGO_REGISTER_COMMAND(ReapLogicalSessionCacheNowCommand).testOnly().forRouter().forShard();

}  // namespace
}  // namespace mongo
