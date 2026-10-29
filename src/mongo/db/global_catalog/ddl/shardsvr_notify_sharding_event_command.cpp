// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

namespace mongo {

class ShardsvrNotifyShardingEventCommand : public TypedCommand<ShardsvrNotifyShardingEventCommand> {
public:
    using Request = ShardsvrNotifyShardingEventRequest;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "should not be calling this directly";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            uassert(ErrorCodes::IllegalOperation,
                    "_shardsvrNotifyShardingEvent can only run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

            // TODO SERVER-100729 Remove the following if clause once 9.0 becomes LTS.
            if (request().getEventType() == notify_sharding_event::kDatabasesAdded) {
                // Ignore the deprecated notification.
                return;
            }

            if (request().getEventType() == notify_sharding_event::kCollectionSharded) {
                const auto event = CollectionSharded::parse(
                    request().getDetails(), IDLParserContext("_shardsvrNotifyShardingEvent"));
                notifyChangeStreamsOnShardCollection(opCtx, event);
                return;
            }

            if (request().getEventType() == notify_sharding_event::kCollectionResharded) {
                const auto event = CollectionResharded::parse(
                    request().getDetails(), IDLParserContext("_shardsvrNotifyShardingEvent"));
                notifyChangeStreamsOnReshardCollectionComplete(opCtx, event);
                return;
            }

            if (request().getEventType() == notify_sharding_event::kNamespacePlacementChanged) {
                const auto event = NamespacePlacementChanged::parse(
                    request().getDetails(), IDLParserContext("_shardsvrNotifyShardingEvent"));
                notifyChangeStreamsOnNamespacePlacementChanged(opCtx, event);
                return;
            }

            if (request().getEventType() ==
                notify_sharding_event::kPlacementHistoryMetadataChanged) {
                const auto event = PlacementHistoryMetadataChanged::parse(
                    request().getDetails(), IDLParserContext("_shardsvrNotifyShardingEvent"));
                notifyChangeStreamsOnPlacementHistoryMetadataChanged(opCtx, event);
                return;
            }

            MONGO_UNREACHABLE_TASSERT(10083526);
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return {};
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrNotifyShardingEventCommand).forShard();

}  // namespace mongo
