/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

/**
 * This command notifies an event on the shard server. The action taken is determined by the
 * event ShardsvrAddShard: Add an oplog entry for the new shard.
 */
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
