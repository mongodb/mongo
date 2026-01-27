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

#include "mongo/db/global_catalog/ddl/timeseries_upgrade_downgrade_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/participant_block_gen.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Returns the list of shards that should receive the timeseries upgrade/downgrade command.
 * - If the collection is tracked, sends to all shards in the cluster.
 * - If the collection is untracked, sends only to the primary shard (this shard).
 */
std::vector<ShardId> getParticipantShards(OperationContext* opCtx, bool isTracked) {
    if (isTracked) {
        return Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    } else {
        return {ShardingState::get(opCtx)->shardId()};
    }
}

}  // namespace

void TimeseriesUpgradeDowngradeCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = TimeseriesUpgradeDowngradeCoordinatorDocument::parse(
        doc, IDLParserContext("TimeseriesUpgradeDowngradeCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getTimeseriesUpgradeDowngradeRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another timeseries upgrade/downgrade operation for namespace "
                          << originalNss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

logv2::DynamicAttributes TimeseriesUpgradeDowngradeCoordinator::getCoordinatorLogAttrs() const {
    return logv2::DynamicAttributes{getBasicCoordinatorAttrs(),
                                    "mode"_attr =
                                        TimeseriesUpgradeDowngradeMode_serializer(_doc.getMode())};
}

/**
 * Early precondition check that runs after the DDL lock is acquired but before acquiring critical
 * section and blocking migrations.
 * Failing here avoids the cost of blocking migrations and acquiring critical sections.
 * The authoritative validation happens in the prepare phase (kPrepareOnShards),
 * which runs canUpgrade/canDowngrade on each shard with the critical section held.

 * Checks performed:
 * - Collection exists and is a timeseries collection
 * - Namespace is the user namespace (not system.buckets)
 * - Feature flag is in correct state for the requested mode
 * - Collection is in expected source format (viewless for downgrade, legacy for upgrade)
 */
void TimeseriesUpgradeDowngradeCoordinator::_checkPreconditions(OperationContext* opCtx) {
    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);

    // Acquire the collection with buckets lookup to handle both legacy and viewless formats.
    auto [collAcquisition, _] = timeseries::acquireCollectionWithBucketsLookup(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, originalNss(), AcquisitionPrerequisites::OperationType::kRead),
        MODE_IS);

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << originalNss().toStringForErrorMsg()
                          << " does not exist",
            collAcquisition.exists());

    const auto& coll = collAcquisition.getCollectionPtr();

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Collection " << originalNss().toStringForErrorMsg()
                          << " is not a timeseries collection",
            coll->isTimeseriesCollection());

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Timeseries upgrade/downgrade must be invoked on the "
                             "time-series user namespace, not on the buckets collection: "
                          << originalNss().toStringForErrorMsg(),
            !originalNss().isTimeseriesBucketsCollection());

    const auto mode = _doc.getTimeseriesUpgradeDowngradeRequest().getMode();

    if (mode == TimeseriesUpgradeDowngradeModeEnum::kToLegacy) {
        tassert(ErrorCodes::IllegalOperation,
                "Cannot downgrade timeseries collection from viewless to legacy when the viewless "
                "timeseries feature flag is enabled",
                !gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
                    VersionContext::getDecoration(opCtx)));
    } else {
        tassert(ErrorCodes::IllegalOperation,
                "Cannot upgrade timeseries collection from legacy to viewless when the viewless "
                "timeseries feature flag is disabled",
                gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
                    VersionContext::getDecoration(opCtx)));
    }

    // Verify the collection is in the expected source format.
    // If upgrading to viewless: should be in legacy format (not viewless).
    // If downgrading to legacy: should be in viewless format.
    const bool isViewless = coll->isNewTimeseriesWithoutView();
    const bool isInExpectedSourceFormat =
        (mode == TimeseriesUpgradeDowngradeModeEnum::kToViewless) ? !isViewless : isViewless;

    uassert(ErrorCodes::RequestAlreadyFulfilled,
            str::stream() << "Collection " << originalNss().toStringForErrorMsg()
                          << " is already in target format",
            isInExpectedSourceFormat);
}

void TimeseriesUpgradeDowngradeCoordinator::_determineIsTracked(OperationContext* opCtx) {
    // Skip if already determined
    if (_doc.getOptTrackedCollInfo().has_value()) {
        return;
    }

    // Fetch and store the collection entry from config.collections if it exists.
    const auto optColl = sharding_ddl_util::getCollectionFromConfigServer(opCtx, nss());
    if (optColl) {
        _doc.setOptTrackedCollInfo(*optColl);
    }
}


bool TimeseriesUpgradeDowngradeCoordinator::_isTracked() const {
    return _doc.getOptTrackedCollInfo().has_value();
}

void TimeseriesUpgradeDowngradeCoordinator::_releaseCriticalSectionFor(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const std::vector<ShardId>& participants,
    const NamespaceString& nss) {
    ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss);
    unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    unblockCRUDOperationsRequest.setReason(_critSecReason);
    unblockCRUDOperationsRequest.setClearFilteringMetadata(true);

    generic_argument_util::setMajorityWriteConcern(unblockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(unblockCRUDOperationsRequest,
                                                   getNewSession(opCtx));

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, unblockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
}

ExecutorFuture<void> TimeseriesUpgradeDowngradeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()]() {
            if (_doc.getPhase() == Phase::kUnset) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                _checkPreconditions(opCtx);
                // Determine whether the collection is tracked.
                _determineIsTracked(opCtx);
            }
        })
        .then(_buildPhaseHandler(
            Phase::kFreezeMigrations,
            [this](OperationContext* opCtx) {
                const bool isTracked = _isTracked();
                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "upgradeDowngradeViewlessTimeseries.start",
                    originalNss(),
                    BSON("mode" << TimeseriesUpgradeDowngradeMode_serializer(_doc.getMode())
                                << "isTracked" << isTracked));
                return isTracked;
            },  // Only for tracked collections
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                LOGV2_DEBUG(11590600,
                            2,
                            "Freezing migrations for timeseries conversion",
                            getCoordinatorLogAttrs());

                // Freeze migrations on both namespaces. One of them will be the tracked namespace
                // and the other will be a no-op since it doesn't exist in the sharding catalog.
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::stopMigrations(opCtx, originalNss(), boost::none, session);
                }
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::stopMigrations(
                        opCtx,
                        originalNss().makeTimeseriesBucketsNamespace(),
                        boost::none,
                        session);
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kAcquireCriticalSection,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const bool isTracked = _isTracked();

                LOGV2_DEBUG(11590601,
                            2,
                            "Acquiring critical section for timeseries conversion",
                            logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                     "isTracked"_attr = isTracked});

                // Block reads and writes on all participating shards for both user namespace and
                // bucket namespace. For untracked collections, this only affects the primary shard.
                auto participants = getParticipantShards(opCtx, isTracked);

                auto acquireCriticalSectionFor = [&](const NamespaceString& ns) {
                    ShardsvrParticipantBlock blockCRUDOperationsRequest(ns);
                    blockCRUDOperationsRequest.setBlockType(
                        CriticalSectionBlockTypeEnum::kReadsAndWrites);
                    blockCRUDOperationsRequest.setReason(_critSecReason);

                    generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
                    generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                                   getNewSession(opCtx));

                    auto opts =
                        std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
                            **executor, token, blockCRUDOperationsRequest);
                    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
                };

                // Acquire critical section on both user namespace and bucket namespace
                acquireCriticalSectionFor(originalNss());
                acquireCriticalSectionFor(originalNss().makeTimeseriesBucketsNamespace());
            }))
        .then(_buildPhaseHandler(
            Phase::kPrepareOnShards,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const bool isTracked = _isTracked();

                LOGV2_DEBUG(11590602,
                            2,
                            "Prepare phase: validating conversion on all shards",
                            logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                     "isTracked"_attr = isTracked});

                auto participants = getParticipantShards(opCtx, isTracked);

                // Send prepare request to all shards to validate they can convert.
                ShardsvrTimeseriesUpgradeDowngradePrepare request(originalNss());
                request.setMode(_doc.getMode());

                generic_argument_util::setMajorityWriteConcern(request);
                generic_argument_util::setOperationSessionInfo(request, getNewSession(opCtx));

                auto opts = std::make_shared<
                    async_rpc::AsyncRPCOptions<ShardsvrTimeseriesUpgradeDowngradePrepare>>(
                    **executor, token, request);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
            }))
        .then(_buildPhaseHandler(
            Phase::kCommitOnShards,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const bool isTracked = _isTracked();

                LOGV2_DEBUG(11590603,
                            2,
                            "Commit phase: applying conversion on all shards",
                            logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                     "isTracked"_attr = isTracked});

                auto participants = getParticipantShards(opCtx, isTracked);

                // Send commit request to all shards to perform the actual conversion.
                ShardsvrTimeseriesUpgradeDowngradeCommit request(originalNss());
                request.setMode(_doc.getMode());
                request.setIsTracked(isTracked);
                request.setDatabasePrimaryShardId(ShardingState::get(opCtx)->shardId());

                generic_argument_util::setMajorityWriteConcern(request);
                generic_argument_util::setOperationSessionInfo(request, getNewSession(opCtx));

                auto opts = std::make_shared<
                    async_rpc::AsyncRPCOptions<ShardsvrTimeseriesUpgradeDowngradeCommit>>(
                    **executor, token, request);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
            }))
        .then(_buildPhaseHandler(
            Phase::kCommitOnGlobalCatalog,
            [this](OperationContext*) { return _isTracked(); },  // Only for tracked collections
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                LOGV2_DEBUG(11590604,
                            2,
                            "Committing timeseries conversion on global catalog",
                            getCoordinatorLogAttrs());

                // Update the namespace in config.collections and bump timestamp/epoch
                // to force routers to refresh.
                const auto& collType = _doc.getOptTrackedCollInfo();

                // Compute new tracked namespace based on mode
                const auto newTrackedNss =
                    (_doc.getMode() == TimeseriesUpgradeDowngradeModeEnum::kToLegacy)
                    ? originalNss().makeTimeseriesBucketsNamespace()
                    : originalNss();

                // Bump collection timestamp and epoch
                auto now = VectorClock::get(opCtx)->getTime();
                auto newTimestamp = now.clusterTime().asTimestamp();
                auto newEpoch = OID::gen();

                // Create new collection document with updated namespace
                // migrations will still be blocked here
                auto newCollType = *collType;
                newCollType.setNss(newTrackedNss);
                newCollType.setTimestamp(newTimestamp);
                newCollType.setEpoch(newEpoch);

                const auto collUuid = collType->getUuid();
                const auto oldTrackedNss = nss();
                const auto osi = getNewSession(opCtx);

                // Get current placement from config.chunks before the transaction
                auto currentShards =
                    sharding_ddl_util::getListOfShardsOwningChunksForCollection(opCtx, collUuid);

                // Execute the namespace change in a transaction
                auto transactionChain = [oldTrackedNss,
                                         newTrackedNss,
                                         collUuid,
                                         newCollType,
                                         newTimestamp,
                                         currentShards](const txn_api::TransactionClient& txnClient,
                                                        ExecutorPtr txnExec) mutable {
                    int stmtId = 1;

                    // Delete the old config.collections entry
                    sharding_ddl_util::deleteTrackedCollectionInTransaction(
                        txnClient, oldTrackedNss, collUuid, stmtId++);

                    // Insert new config.collections entry with updated namespace
                    sharding_ddl_util::upsertTrackedCollectionInTransaction(
                        txnClient, newCollType, stmtId++);

                    // Update zone assignments (config.tags) to use new namespace
                    sharding_ddl_util::updateZonesInTransaction(
                        txnClient, oldTrackedNss, newTrackedNss);

                    // Update placement history:
                    // - OLD namespace gets empty shards (no longer tracked there)
                    // - NEW namespace gets the actual shards
                    sharding_ddl_util::upsertPlacementHistoryDocInTransaction(
                        txnClient, oldTrackedNss, collUuid, newTimestamp, {}, stmtId++);
                    sharding_ddl_util::upsertPlacementHistoryDocInTransaction(
                        txnClient,
                        newTrackedNss,
                        collUuid,
                        newTimestamp,
                        std::move(currentShards),
                        stmtId++);

                    return SemiFuture<void>::makeReady();
                };

                sharding_ddl_util::runTransactionOnShardingCatalog(opCtx,
                                                                   std::move(transactionChain),
                                                                   defaultMajorityWriteConcern(),
                                                                   osi,
                                                                   **executor);

                // TODO SERVER-117481: Generate placement change notifications for change streams.
                // Similar to rename coordinator (kSetupChangeStreamsPreconditions phase), we need
                // to notify change stream readers about the namespace change from oldTrackedNss
                // to newTrackedNss using generatePlacementChangeNotificationOnShard().
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCriticalSection,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const bool isTracked = _isTracked();

                LOGV2_DEBUG(11590619,
                            2,
                            "Releasing critical section for timeseries conversion",
                            logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                     "isTracked"_attr = isTracked});

                // Release critical section on all participating shards.
                // For untracked collections, this only affects the primary shard.
                auto participants = getParticipantShards(opCtx, isTracked);

                // Release critical section on both namespaces
                _releaseCriticalSectionFor(opCtx,
                                           executor,
                                           token,
                                           participants,
                                           originalNss().makeTimeseriesBucketsNamespace());
                _releaseCriticalSectionFor(opCtx, executor, token, participants, originalNss());
            }))
        .then(_buildPhaseHandler(
            Phase::kResumeMigrations,
            [this](OperationContext*) { return _isTracked(); },  // Only for tracked collections
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                LOGV2_DEBUG(11590607,
                            2,
                            "Resuming migrations after timeseries conversion",
                            getCoordinatorLogAttrs());

                // Resume migrations on both namespaces. One of them will be the tracked namespace
                // and the other will be a no-op since it doesn't exist in the sharding catalog.
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::resumeMigrations(opCtx, originalNss(), boost::none, session);
                }
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::resumeMigrations(
                        opCtx,
                        originalNss().makeTimeseriesBucketsNamespace(),
                        boost::none,
                        session);
                }
            }))
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            const bool isTracked = _isTracked();
            ShardingLogging::get(opCtx)->logChange(
                opCtx,
                "upgradeDowngradeViewlessTimeseries.end",
                originalNss(),
                BSON("mode" << TimeseriesUpgradeDowngradeMode_serializer(_doc.getMode())
                            << "isTracked" << isTracked));
        })
        .onError<ErrorCodes::RequestAlreadyFulfilled>(
            [this, anchor = shared_from_this()](const Status& status) { return Status::OK(); })
        .onError([this, executor, anchor = shared_from_this()](const Status& status) {
            // From kCommitOnShards onwards, we've done irreversible work on the shards.
            // We must keep retrying to complete the operation, not rollback.
            // For retriable errors at any phase, just return and let the base class retry.
            if (_doc.getPhase() >= Phase::kCommitOnShards ||
                _isRetriableErrorForDDLCoordinator(status)) {
                return status;
            }

            // Between kFreezeMigrations and kCommitOnShards (exclusive), we can rollback.
            // Trigger cleanup to undo freeze migrations and release critical section.
            if (_doc.getPhase() >= Phase::kFreezeMigrations) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                triggerCleanup(opCtx, status);
                MONGO_UNREACHABLE_TASSERT(11590618);
            }

            return status;
        });
}

ExecutorFuture<void> TimeseriesUpgradeDowngradeCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            const auto& failedPhase = _doc.getPhase();

            // From kCommitOnShards onwards, we've done irreversible work on the shards and must
            // always make progress. We should never enter cleanup from that point.
            tassert(
                11590620,
                str::stream() << "Unexpected cleanup at phase "
                              << TimeseriesUpgradeDowngradeCoordinatorPhase_serializer(failedPhase)
                              << " which is >= kCommitOnShards",
                failedPhase < Phase::kCommitOnShards);

            // If we failed before kFreezeMigrations, we haven't done any work that needs cleanup.
            if (failedPhase < Phase::kFreezeMigrations) {
                return;
            }

            const bool isTracked = _isTracked();

            // Release critical section if it was acquired.
            if (failedPhase >= Phase::kAcquireCriticalSection) {
                LOGV2_DEBUG(
                    11590609,
                    2,
                    "Releasing critical section on abort",
                    logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                             "failedPhase"_attr = serializePhase(failedPhase),
                                             "isTracked"_attr = isTracked});

                auto participants = getParticipantShards(opCtx, isTracked);

                // Release critical section on both namespaces.
                _releaseCriticalSectionFor(opCtx,
                                           executor,
                                           token,
                                           participants,
                                           originalNss().makeTimeseriesBucketsNamespace());
                _releaseCriticalSectionFor(opCtx, executor, token, participants, originalNss());
            }

            // Resume migrations if they were frozen (only for tracked collections).
            if (isTracked && failedPhase >= Phase::kFreezeMigrations) {
                LOGV2_DEBUG(
                    11590610,
                    2,
                    "Resuming migrations on abort",
                    logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                             "failedPhase"_attr = serializePhase(failedPhase)});

                // Resume migrations on both namespaces. One of them will be the tracked namespace
                // and the other will be a no-op since it doesn't exist in the sharding catalog.
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::resumeMigrations(opCtx, originalNss(), boost::none, session);
                }
                {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::resumeMigrations(
                        opCtx,
                        originalNss().makeTimeseriesBucketsNamespace(),
                        boost::none,
                        session);
                }
            }
        });
}

}  // namespace mongo
