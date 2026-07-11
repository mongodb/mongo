// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class FsyncCommand : public ErrmsgCommandDeprecated {
public:
    FsyncCommand() : ErrmsgCommandDeprecated("fsync") {}

    std::string help() const override {
        return "invoke fsync on all shards belonging to the cluster";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::fsync)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    void unlockLockedShards(OperationContext* opCtx, const DatabaseName& dbname) {
        auto request = OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::get(opCtx), dbname, BSON("fsyncUnlock" << 1));
        auto response = CommandHelpers::runCommandDirectly(opCtx, request);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const DatabaseName& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        BSONObj fsyncCmdObj = cmdObj;

        if (cmdObj["lock"].trueValue()) {
            auto forBackupField = BSON("forBackup" << true);
            fsyncCmdObj = fsyncCmdObj.addFields(forBackupField);
        }

        auto shardResults = scatterGatherUnversionedTargetConfigServerAndShards(
            opCtx,
            dbName,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(fsyncCmdObj)),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);

        BSONObjBuilder rawResult;
        const auto response = appendRawResponses(opCtx, &errmsg, &rawResult, shardResults);

        // This field has had dummy value since MMAP went away. It is undocumented.
        // Maintaining it so as not to cause unnecessary user pain across upgrades.
        result.append("numFiles", 1);
        result.append("all", rawResult.obj());
        if (!response.responseOK) {
            if (cmdObj["lock"].trueValue()) {
                unlockLockedShards(opCtx, dbName);
            }
            return false;
        }

        return true;
    }
};
MONGO_REGISTER_COMMAND(FsyncCommand).forRouter();

}  // namespace
}  // namespace mongo
