// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/testing_proctor.h"

#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrCheckMetadataConsistencySecondaryParticipantCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencySecondaryParticipantCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistencySecondaryParticipant;
    using Response = CursorInitialReply;

    bool adminOnly() const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            tassert(12922000,
                    fmt::format("{} is a test-only command", Request::kCommandName),
                    TestingProctor::instance().isEnabled());

            const auto hostAndPort = repl::ReplicationCoordinator::get(opCtx)->getMyHostAndPort();
            uassert(ErrorCodes::NotYetInitialized,
                    "Replication is not initialized",
                    !hostAndPort.empty());

            const auto nss = ns();
            const auto& primaryShardId = request().getPrimaryShardId();
            const auto checkRangeDeletionIndexes =
                request().getCommonFields().getCheckRangeDeletionIndexes();
            const auto checkIndexes = request().getCommonFields().getCheckIndexes();

            auto inconsistencies =
                metadata_consistency_util::runCheckMetadataConsistencyOnParticipant(
                    opCtx,
                    nss,
                    primaryShardId,
                    checkRangeDeletionIndexes,
                    checkIndexes,
                    false /* asRSPrimaryNode */);

            const auto& shardId = ShardingState::get(opCtx)->shardId();
            for (auto& inconsistency : inconsistencies) {
                inconsistency.getProvenance().emplace(shardId, hostAndPort);
            }

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx, nss, std::move(inconsistencies), request().getCursor(), request().toBSON());
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            if (level == repl::ReadConcernLevel::kSnapshotReadConcern) {
                return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
            } else {
                return CommandInvocation::supportsReadConcern(level, isImplicitDefault);
            }
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
MONGO_REGISTER_COMMAND(ShardsvrCheckMetadataConsistencySecondaryParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
