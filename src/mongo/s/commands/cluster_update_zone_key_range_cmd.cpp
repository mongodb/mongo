// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/s/request_types/update_zone_key_range_gen.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

class UpdateZoneKeyRangeCmd : public TypedCommand<UpdateZoneKeyRangeCmd> {
public:
    using Request = UpdateZoneKeyRange;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());

            BSONObjBuilder cmdBuilder;
            ConfigsvrUpdateZoneKeyRange cmd(
                ns(), request().getMin(), request().getMax(), request().getZone());
            generic_argument_util::setMajorityWriteConcern(cmd);
            cmd.serialize(&cmdBuilder);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponseStatus =
                uassertStatusOK(configShard->runCommand(opCtx,
                                                        kPrimaryOnlyReadPreference,
                                                        DatabaseName::kAdmin,
                                                        cmdBuilder.obj(),
                                                        Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(cmdResponseStatus.commandStatus);
        }

    private:
        bool isRemove() const {
            if (request().getZone()) {
                return false;
            }
            return true;
        }

        ChunkRange getRange() const {
            BSONObj minKey = request().getMin();
            BSONObj maxKey = request().getMax();

            uassertStatusOK(ChunkRange::validate(minKey, maxKey));
            return ChunkRange(minKey, maxKey);
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* as = AuthorizationSession::get(opCtx->getClient());

            if (as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                    ActionType::enableSharding)) {
                return;
            }

            // Fallback on permissions to directly modify the shard config.
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString::kConfigsvrShardsNamespace),
                    ActionType::find));

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(TagsType::ConfigNS), ActionType::find));

            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(TagsType::ConfigNS), ActionType::update));
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(TagsType::ConfigNS), ActionType::remove));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Assigns/removes a range of a sharded collection to a zone.";
    }
};
MONGO_REGISTER_COMMAND(UpdateZoneKeyRangeCmd).forRouter();

}  // namespace
}  // namespace mongo
