// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {
namespace {

class RefreshLogicalSessionCacheNowCommand final : public BasicCommand {
public:
    RefreshLogicalSessionCacheNowCommand() : BasicCommand("refreshLogicalSessionCacheNow") {}

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
        return "force the logical session cache to refresh. Test command only.";
    }

    bool requiresAuth() const override {
        return false;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto cache = LogicalSessionCache::get(opCtx);

        auto res = cache->refreshNow(opCtx);
        if (res.code() != ErrorCodes::DuplicateKey) {
            uassertStatusOK(res);
        }

        return true;
    }
};

MONGO_REGISTER_COMMAND(RefreshLogicalSessionCacheNowCommand).testOnly().forRouter().forShard();

}  // namespace
}  // namespace mongo
