/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
                auto newClient = getGlobalServiceContext()
                                     ->getService(ClusterRole::ShardServer)
                                     ->makeClient("ShardsvrTimeseriesUpgradeDowngradeCommit");
                AlternativeClientRegion acr(newClient);
                auto newOpCtx = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
                newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

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
