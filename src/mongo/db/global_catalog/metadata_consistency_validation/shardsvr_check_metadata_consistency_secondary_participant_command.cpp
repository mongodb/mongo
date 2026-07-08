/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
