// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/out_of_line_executor.h"

#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Checks if a tracked collection exists locally on this shard. A shard may not have a local copy of
 * a sharded collection if it never owned any chunks for that collection.
 *
 * We use lookupCollectionByNamespace instead of establishConsistentCollection because this runs
 * inside a sharded DDL operation that holds the DDL lock.
 * The two only differ when not holding locks and concurrent DDLs could commit changes to the
 * collection.
 */
bool collectionExistsLocally(OperationContext* opCtx, const NamespaceString& nss) {
    return CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss) != nullptr;
}


/**
 * Prepare command: validates that the conversion can be performed on this shard.
 */
class ShardsvrTimeseriesUpgradeDowngradePrepareCommand final
    : public TypedCommand<ShardsvrTimeseriesUpgradeDowngradePrepareCommand> {
public:
    using Request = ShardsvrTimeseriesUpgradeDowngradePrepare;

    std::string help() const override {
        return "Internal command to validate timeseries upgrade/downgrade feasibility. "
               "Do not call directly.";
    }

    bool skipApiVersionCheck() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            const auto& nss = ns();
            const bool isUpgrade =
                request().getMode() == TimeseriesUpgradeDowngradeModeEnum::kToViewless;

            // Check if the source collection exists locally. For upgrade, source is
            // system.buckets.X; for downgrade, source is X. A tracked collection may not exist
            // locally if this shard never owned any chunks for it (or all chunks were migrated
            // away). This is different from idempotency (already converted), which
            // canUpgrade/canDowngrade handles by checking if the target format already exists.
            const auto bucketsNss = nss.makeTimeseriesBucketsNamespace();
            const auto& sourceNss = isUpgrade ? bucketsNss : nss;
            if (!collectionExistsLocally(opCtx, sourceNss)) {
                LOGV2_DEBUG(11590611,
                            1,
                            "Timeseries upgrade/downgrade prepare: collection not found locally, "
                            "skipping validation",
                            logAttrs(nss),
                            "isUpgrade"_attr = isUpgrade);
                return;
            }

            Status validationStatus = isUpgrade
                ? timeseries::canUpgradeToViewlessTimeseries(opCtx, nss)
                : timeseries::canDowngradeFromViewlessTimeseries(opCtx, nss);
            uassertStatusOK(validationStatus);

            LOGV2_DEBUG(11590612,
                        1,
                        "Timeseries upgrade/downgrade prepare phase completed successfully",
                        logAttrs(nss),
                        "isUpgrade"_attr = isUpgrade);
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
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
MONGO_REGISTER_COMMAND(ShardsvrTimeseriesUpgradeDowngradePrepareCommand).forShard();

/**
 * Commit command: performs the actual timeseries upgrade/downgrade on this shard.
 */
class ShardsvrTimeseriesUpgradeDowngradeCommitCommand final
    : public TypedCommand<ShardsvrTimeseriesUpgradeDowngradeCommitCommand> {
public:
    using Request = ShardsvrTimeseriesUpgradeDowngradeCommit;

    std::string help() const override {
        return "Internal command to perform timeseries upgrade/downgrade. Do not call directly.";
    }

    bool skipApiVersionCheck() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " expected to be called within a retryable write",
                    TransactionParticipant::get(opCtx));

            const auto& nss = ns();
            const bool isTracked = request().getIsTracked();
            const bool isUpgrade =
                request().getMode() == TimeseriesUpgradeDowngradeModeEnum::kToViewless;

            // During downgrade, only the primary shard should create the view.
            // The coordinator sends its shard ID in the request, allowing each participant to
            // compare it with their own shard ID to determine if they are the primary shard.
            const bool isPrimaryShard =
                request().getDatabasePrimaryShardId() == ShardingState::get(opCtx)->shardId();

            {
                // Using the original operation context, operations that use PersistentTaskStore
                // (like range deletion utilities) would fail since retryable writes cannot have
                // limit=0. A tactical solution is to use an alternative client as well as a new
                // operation context.
                auto newClient = getGlobalServiceContext()->getService()->makeClient(
                    "ShardsvrTimeseriesUpgradeDowngradeCommit");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                // Mark the alternative opCtx non-deprioritizable
                mongo::admission::execution_control::ScopedTaskTypeNonDeprioritizable
                    altDeprioGuard(newOpCtx.get());

                ForwardableOperationMetadata forwardableOpMetadata(opCtx);
                forwardableOpMetadata.setOn(newOpCtx.get());

                if (isTracked) {
                    // For tracked collections, we need to update range deletion tasks to use the
                    // new tracked namespace. This mirrors what rename does.
                    const auto bucketsNss = nss.makeTimeseriesBucketsNamespace();

                    // Determine old and new tracked namespaces based on conversion direction:
                    // - kToViewless (upgrade): system.buckets.ts -> ts
                    // - kToLegacy (downgrade): ts -> system.buckets.ts
                    const auto& oldTrackedNss = isUpgrade ? bucketsNss : nss;
                    const auto& newTrackedNss = isUpgrade ? nss : bucketsNss;

                    // Only perform conversion if the collection exists locally. A shard may not
                    // have a local copy if it never owned any chunks.
                    if (collectionExistsLocally(newOpCtx.get(), oldTrackedNss)) {
                        // Snapshot range deletion tasks before conversion.
                        rangedeletionutil::snapshotRangeDeletionsForRename(
                            newOpCtx.get(), oldTrackedNss, newTrackedNss);

                        // Perform the local conversion.
                        // For downgrade, only the primary shard creates the view.
                        if (isUpgrade) {
                            timeseries::upgradeToViewlessTimeseries(newOpCtx.get(), nss);
                        } else {
                            const bool skipViewCreation = !isPrimaryShard;
                            timeseries::downgradeFromViewlessTimeseries(
                                newOpCtx.get(),
                                nss,
                                boost::none /* expectedUUID */,
                                skipViewCreation);
                        }

                        // Restore range deletion tasks with the new namespace and clean up.
                        rangedeletionutil::restoreRangeDeletionTasksForRename(newOpCtx.get(),
                                                                              newTrackedNss);
                        rangedeletionutil::deleteRangeDeletionTasksForRename(
                            newOpCtx.get(), oldTrackedNss, newTrackedNss);
                    }
                } else {
                    // For untracked collections, there are no range deletion tasks to handle
                    // since they never go through chunk migrations. Just perform the conversion.
                    // Note: untracked collections only exist on the primary shard, so view is
                    // always created.
                    if (isUpgrade) {
                        timeseries::upgradeToViewlessTimeseries(newOpCtx.get(), nss);
                    } else {
                        timeseries::downgradeFromViewlessTimeseries(newOpCtx.get(), nss);
                    }
                }
            }

            // Since no write that generated a retryable write oplog entry with this sessionId and
            // txnNumber happened, we need to make a dummy write so that the session gets durably
            // persisted on the oplog. This must be the last operation done on this command.
            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kServerConfigurationNamespace,
                            BSON("_id" << Request::kCommandName),
                            BSON("$inc" << BSON("count" << 1)),
                            true /* upsert */,
                            false /* multi */);
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
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
MONGO_REGISTER_COMMAND(ShardsvrTimeseriesUpgradeDowngradeCommitCommand).forShard();

}  // namespace
}  // namespace mongo
