// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/shard_role/ddl/coll_mod_reply_validation.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrCollModCommand final : public BasicCommandWithRequestParser<ShardsvrCollModCommand> {
public:
    using Request = ShardsvrCollMod;
    using Response = CollModReply;

    ShardsvrCollModCommand() : BasicCommandWithRequestParser() {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Modifies collection.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* client = opCtx->getClient();
        const NamespaceString nss(parseNs(dbName, cmdObj));
        return auth::checkAuthForCollMod(client->getOperationContext(),
                                         AuthorizationSession::get(client),
                                         nss,
                                         cmdObj,
                                         false,
                                         SerializationContext::stateCommandRequest());
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) override {
        auto const shardingState = ShardingState::get(opCtx);
        shardingState->assertCanAcceptShardedCommands();

        CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                      opCtx->getWriteConcern());

        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        // Since this operation is not directly writing locally we need to force its db
        // profile level increase in order to be logged in "<db>.system.profile"
        const auto& cmd = requestParser.request();
        CurOp::get(opCtx)->raiseDbProfileLevel(
            DatabaseProfileSettings::get(opCtx->getServiceContext())
                .getDatabaseProfileLevel(cmd.getNamespace().dbName()));

        // FixedFCVRegion must be released before getResult() to avoid a deadlock: getResult()
        // waits for the coordinator to complete, and the coordinator may need to acquire the
        // FCV lock exclusively.
        auto collModCoordinator = [&]() {
            FixedFCVRegion fixedFcvRegion(opCtx);

            const auto& collModRequest = cmd.getCollModRequest();
            if (collModRequest.getValidationLevel() == ValidationLevelEnum::constraint ||
                collModRequest.getPrepareConstraintValidationLevel()) {
                const auto fcvSnapshot =
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
                uassert(
                    ErrorCodes::InvalidOptions,
                    "Validation level 'constraint' is not supported with current FCV",
                    gFeatureFlagConstraintValidationLevel.isEnabledUseLastLTSFCVWhenUninitialized(
                        VersionContext::getDecoration(opCtx), fcvSnapshot));
            }

            auto coordinatorDoc = CollModCoordinatorDocument();
            coordinatorDoc.setCollModRequest(collModRequest);
            coordinatorDoc.setShardingCoordinatorMetadata(
                {{cmd.getNamespace(), CoordinatorTypeEnum::kCollMod}});
            auto service = ShardingCoordinatorService::getService(opCtx);
            return checked_pointer_cast<CollModCoordinator>(
                service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON(), fixedFcvRegion));
        }();

        result.appendElements(collModCoordinator->getResult(opCtx));

        if (const auto level = cmd.getCollModRequest().getValidationLevel()) {
            validationLevelCounters.increment(CollMod::kCommandName, *level);
        }

        return true;
    }

    void validateResult(const BSONObj& resultObj) final {
        StringDataSet ignorableFields({"raw", "ok", "errmsg"});
        auto reply = Response::parse(resultObj.removeFields(ignorableFields),
                                     IDLParserContext("CollModReply"));
        coll_mod_reply_validation::validateReply(reply);
    }
};
MONGO_REGISTER_COMMAND(ShardsvrCollModCommand).forShard();

}  // namespace
}  // namespace mongo
