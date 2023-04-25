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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/commands/cluster_commands_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_range_request_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by mongod and
                                                // writeConcernMajorityJournalDefault is set to true
                                                // in the ReplSetConfig.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutSharding);

class ShardsvrMoveRangeCommand final : public TypedCommand<ShardsvrMoveRangeCommand> {
public:
    using Request = ShardsvrMoveRange;

    ShardsvrMoveRangeCommand() : TypedCommand<ShardsvrMoveRangeCommand>(Request::kCommandName) {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command invoked by the config server to move a chunk/range";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            // Make sure we're as up-to-date as possible with shard information. This catches the
            // case where we might have changed a shard's host by removing/adding a shard with the
            // same name.
            Grid::get(opCtx)->shardRegistry()->reload(opCtx);

            auto scopedMigration = uassertStatusOK(
                ActiveMigrationsRegistry::get(opCtx).registerDonateChunk(opCtx, request()));

            // Check if there is an existing migration running and if so, join it
            if (scopedMigration.mustExecute()) {
                auto moveChunkComplete =
                    ExecutorFuture<void>(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor())
                        .then([req = request(),
                               writeConcern = opCtx->getWriteConcern(),
                               scopedMigration = std::move(scopedMigration),
                               serviceContext = opCtx->getServiceContext()]() mutable {
                            // This local variable is created to enforce that the scopedMigration is
                            // destroyed before setting the shared state as ready.
                            // Note that captured objects of the lambda are destroyed by the
                            // executor thread after setting the shared state as ready.
                            auto scopedMigrationLocal(std::move(scopedMigration));
                            ThreadClient tc("MoveChunk", serviceContext);
                            {
                                stdx::lock_guard<Client> lk(*tc.get());
                                tc->setSystemOperationKillableByStepdown(lk);
                            }
                            auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
                            auto executorOpCtx = uniqueOpCtx.get();
                            Status status = {ErrorCodes::InternalError, "Uninitialized value"};
                            try {
                                executorOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
                                {
                                    // Ensure that opCtx will get interrupted in the event of a
                                    // stepdown. This is to ensure that the MigrationSourceManager
                                    // checks that there are no pending migrationCoordinators
                                    // documents (under the ActiveMigrationRegistry lock) on the
                                    // same term during which the migrationCoordinators document
                                    // will be persisted.
                                    Lock::GlobalLock lk(executorOpCtx, MODE_IX);
                                    uassert(ErrorCodes::InterruptedDueToReplStateChange,
                                            "Not primary while attempting to start chunk migration "
                                            "donation",
                                            repl::ReplicationCoordinator::get(executorOpCtx)
                                                ->getMemberState()
                                                .primary());
                                }
                                // Note: This internal authorization is tied to the lifetime of the
                                // client.
                                AuthorizationSession::get(executorOpCtx->getClient())
                                    ->grantInternalAuthorization(executorOpCtx->getClient());
                                _runImpl(executorOpCtx, std::move(req), std::move(writeConcern));
                                status = Status::OK();
                            } catch (const DBException& e) {
                                status = e.toStatus();
                                LOGV2_WARNING(23777,
                                              "Chunk move failed with {error}",
                                              "Error while doing moveChunk",
                                              "error"_attr = redact(status));

                                if (status.code() == ErrorCodes::LockTimeout) {
                                    ShardingStatistics::get(executorOpCtx)
                                        .countDonorMoveChunkLockTimeout.addAndFetch(1);
                                }
                            }

                            scopedMigrationLocal.signalComplete(status);
                            uassertStatusOK(status);
                        });
                moveChunkComplete.get(opCtx);
            } else {
                uassertStatusOK(scopedMigration.waitForCompletion(opCtx));
            }

            if (request().getWaitForDelete()) {
                // Ensure we capture the latest opTime in the system, since range deletion happens
                // asynchronously with a different OperationContext. This must be done after the
                // above join, because each caller must set the opTime to wait for writeConcern for
                // on its own OperationContext.
                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                replClient.setLastOpToSystemLastOpTime(opCtx);

                WriteConcernResult writeConcernResult;
                Status majorityStatus = waitForWriteConcern(
                    opCtx, replClient.getLastOp(), kMajorityWriteConcern, &writeConcernResult);

                uassertStatusOKWithContext(
                    majorityStatus, "Failed to wait for range deletions after migration commit");
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }

        static void _runImpl(OperationContext* opCtx,
                             ShardsvrMoveRange&& request,
                             WriteConcernOptions&& writeConcern) {
            if (request.getFromShard() == request.getToShard()) {
                return;
            }

            // Resolve the donor and recipient shards and their connection string
            auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

            const auto donorConnStr =
                uassertStatusOK(shardRegistry->getShard(opCtx, request.getFromShard()))
                    ->getConnString();
            const auto recipientHost = uassertStatusOK([&] {
                auto recipientShard =
                    uassertStatusOK(shardRegistry->getShard(opCtx, request.getToShard()));

                return recipientShard->getTargeter()->findHost(
                    opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
            }());

            MigrationSourceManager migrationSourceManager(
                opCtx, std::move(request), std::move(writeConcern), donorConnStr, recipientHost);

            migrationSourceManager.startClone();
            migrationSourceManager.awaitToCatchUp();
            migrationSourceManager.enterCriticalSection();
            migrationSourceManager.commitChunkOnRecipient();
            migrationSourceManager.commitChunkMetadataOnConfig();
        }
    };

} _shardsvrMoveRangeCmd;

}  // namespace
}  // namespace mongo
