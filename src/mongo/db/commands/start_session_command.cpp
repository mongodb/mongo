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
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"

#include <string>

namespace mongo {
namespace {

class StartSessionCommand final : public BasicCommand {
    StartSessionCommand(const StartSessionCommand&) = delete;
    StartSessionCommand& operator=(const StartSessionCommand&) = delete;

public:
    StartSessionCommand() : BasicCommand("startSession") {}

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
        return "start a logical session";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto service = opCtx->getServiceContext();
        const auto lsCache = LogicalSessionCache::get(service);

        auto newSessionRecord =
            makeLogicalSessionRecord(opCtx, service->getFastClockSource()->now());

        uassertStatusOK(lsCache->startSession(opCtx, newSessionRecord));

        makeLogicalSessionToClient(newSessionRecord.getId()).serialize(&result);

        return true;
    }
};
MONGO_REGISTER_COMMAND(StartSessionCommand).forRouter().forShard();

}  // namespace
}  // namespace mongo
