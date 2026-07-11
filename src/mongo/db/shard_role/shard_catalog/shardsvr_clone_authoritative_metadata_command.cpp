// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/clone_authoritative_metadata_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrCloneAuthoritativeMetadataCommand final
    : public TypedCommand<ShardsvrCloneAuthoritativeMetadataCommand> {
public:
    using Request = ShardsvrCloneAuthoritativeMetadata;

    std::string help() const override {
        return "Internal command, do not invoke directly.";
    }

    bool adminOnly() const override {
        return true;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            // Ensure that the vector clock has been persisted on disk; the DDL coordinator will
            // later issue potentially secondary reads to the config server requesting the last
            // known config time as the "Majority Read Concern" value; such queries could return
            // data that are not causally consistent with the invocation of this command from the
            // config server when 1) the coordinator has not an up-to-date configTime value (for
            // example, as a consequence of recently becoming a primary) and 2) the queries are
            // received by a lagging secondary config server node.
            VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

            boost::optional<FixedFCVRegion> fcvRegion{boost::in_place_init, opCtx};

            const auto accessLevel = sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                VersionContext::getDecoration(opCtx), fcvRegion.get()->acquireFCVSnapshot());
            tassert(12806400,
                    "CloneAuthoritativeMetadata invoked on a fully non-authoritative shard",
                    accessLevel >= AuthoritativeMetadataAccessLevelEnum::kWritesAllowed);
            // If the shard is already authoritative, return OK by idempotency.
            // Cloning again is unsafe due to concurrent reads, chunk migrations, etc.
            // Note we can not tassert here; see SERVER-128064 for how this path may be reached.
            if (accessLevel == AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed) {
                LOGV2(12806401,
                      "Skipping CloneAuthoritativeMetadata: shard is already authoritative",
                      "accessLevel"_attr = idlSerialize(accessLevel));
                return;
            }

            auto coordinatorDoc = CloneAuthoritativeMetadataCoordinatorDocument();
            coordinatorDoc.setShardingCoordinatorMetadata(
                {{NamespaceString::kConfigShardCatalogDatabasesNamespace,
                  CoordinatorTypeEnum::kCloneAuthoritativeMetadata}});

            auto service = ShardingCoordinatorService::getService(opCtx);
            auto coordinator = checked_pointer_cast<CloneAuthoritativeMetadataCoordinator>(
                service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON(), *fcvRegion));
            // Release the FCV region while the coordinator executes
            fcvRegion.reset();

            coordinator->getCompletionFuture().get(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return true;
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
MONGO_REGISTER_COMMAND(ShardsvrCloneAuthoritativeMetadataCommand)
    .requiresFeatureFlag(feature_flags::gAuthoritativeShardsDDL)
    .forShard();

}  // namespace
}  // namespace mongo
