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

#include "mongo/s/write_ops/unified_write_executor/write_batch_scheduler.h"

#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/router_role/router_role.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {
namespace {

// Returns true if the routing cache produced new metadata relative to the stale versions observed
// in the previous round. Checks both StaleConfig (shard-specific ShardVersion per namespace) and
// StaleDbVersion (DatabaseVersion per database) errors.
bool checkMetadataRefreshed(
    const RoutingContext& routingCtx,
    const std::vector<NamespaceString>& nssList,
    const stdx::unordered_map<NamespaceString, std::pair<ShardId, ShardVersion>>& prevStaleVersions,
    const stdx::unordered_map<DatabaseName, DatabaseVersion>& prevStaleDbVersions) {
    // Check StaleConfig: compare shard-specific ShardVersions (not the collection-level max) to
    // avoid false positives on multi-shard collections.
    for (const auto& [nss, shardAndVersion] : prevStaleVersions) {
        const auto& [shardId, prevVersion] = shardAndVersion;
        if (!routingCtx.hasNss(nss)) {
            // StaleConfig errors can carry the internal buckets namespace (system.buckets.*)
            // rather than the user-facing timeseries namespace, which is what _nssSet tracks.
            tassert(12378201,
                    "NSS from StaleConfig error not found in RoutingContext and is not a "
                    "timeseries buckets namespace",
                    nss.isTimeseriesBucketsCollection());
            continue;
        }
        const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
        if (!cri.hasRoutingTable()) {
            // Guard before getShardVersion() to avoid tassert when the collection has no routing
            // table. The transition from tracked (prevVersion != UNTRACKED) to untracked is a
            // productive refresh. Once the routing cache has already settled on untracked
            // (prevVersion == UNTRACKED), further StaleConfig rounds are not productive.
            if (prevVersion != ShardVersion::UNTRACKED()) {
                return true;
            }
        } else if (cri.getShardVersion(shardId) != prevVersion) {
            return true;
        }
    }

    // Check StaleDbVersion: for each affected database, find any namespace in that database and
    // compare DatabaseVersions. A change in DatabaseVersion means the primary shard changed, which
    // is a productive refresh.
    for (const auto& [dbName, prevDbVersion] : prevStaleDbVersions) {
        for (const auto& nss : nssList) {
            if (nss.dbName() != dbName) {
                continue;
            }
            const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
            if (cri.getDbVersion() != prevDbVersion) {
                return true;
            }
            break;  // one NSS per database is enough
        }
    }

    return false;
}

}  // namespace

void WriteBatchScheduler::run(OperationContext* opCtx) {
    // The loop below uses an exponential backoff scheme that kicks in when there are one or more
    // consecutive rounds that don't make progress. If there are too many consecutive rounds without
    // progress, this function will eventually give up and fail with an error. These variables are
    // used to implement this mechanism.
    int32_t rounds = 0;
    int32_t numRoundsWithoutProgress = 0;
    Backoff backoff(Seconds(1), Seconds(2));

    // Keep executing rounds until the batcher says it can't make any more batches or until an
    // unrecoverable error or exception occurs.
    while (!_batcher.isDone()) {
        // If there have been too many consecutive rounds without progress, record an error for
        // the remaining ops and break out of the loop.
        if (numRoundsWithoutProgress > _kMaxRoundsWithoutProgress) {
            Status status{ErrorCodes::NoProgressMade,
                          str::stream() << "No progress was made executing write ops in after "
                                        << _kMaxRoundsWithoutProgress << " rounds (" << rounds
                                        << " rounds total)"};
            recordErrorForRemainingOps(opCtx, status);
            break;
        }

        CurOp::get(opCtx)->maybeLogSlowQuery();

        // If no progress was made during the previous round, do exponential backoff before
        // starting this round.
        if (numRoundsWithoutProgress > 0) {
            sleepFor(backoff.nextSleep());
        }

        // Execute a round.
        auto result = executeRound(opCtx);

        // Increment 'rounds', update 'numRoundsWithoutProgress', and print a message to the log.
        ++rounds;
        numRoundsWithoutProgress =
            (result.madeProgress || result.metadataRefreshed) ? 0 : numRoundsWithoutProgress + 1;
        LOGV2_DEBUG(10896504, 4, "Completed round", "rounds completed"_attr = rounds);
    }
}

WriteBatchScheduler::RoundResult WriteBatchScheduler::executeRound(OperationContext* opCtx) {
    const bool ordered = _cmdRef.getOrdered();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const auto nssList = std::vector<NamespaceString>{_nssSet.begin(), _nssSet.end()};

    // Take ownership of the previous round's stale versions so we can compare them against the
    // new RoutingContext below. Both maps will be repopulated after this round.
    auto prevStaleVersions = std::move(_prevStaleVersions);
    auto prevStaleDbVersions = std::move(_prevStaleDbVersions);

    // If we've exceeded our memory limit, stop execution. _prevStaleVersions is intentionally
    // not restored here because stopMakingBatches() guarantees no further rounds will run.
    if (_processor.checkBulkWriteReplyMaxSize(opCtx)) {
        _batcher.stopMakingBatches();
        return RoundResult{.madeProgress = false, .metadataRefreshed = false};
    }

    // Create a RoutingContext. If creating the RoutingContext fails, handle the failure and
    // return early. _prevStaleVersions is intentionally not restored here because
    // handleInitRoutingContextError() either throws or calls stopMakingBatches(), guaranteeing
    // no further rounds will run.
    auto swRoutingCtx = initRoutingContext(opCtx, nssList);
    if (!swRoutingCtx.isOK()) {
        handleInitRoutingContextError(opCtx, swRoutingCtx.getStatus());
        return RoundResult{.madeProgress = false, .metadataRefreshed = false};
    }

    bool metadataRefreshed = checkMetadataRefreshed(
        *swRoutingCtx.getValue(), nssList, prevStaleVersions, prevStaleDbVersions);

    // Capture how many OK responses there have been at the start of the round so we can compare
    // against it when the round has finished.
    const size_t previousNumOkItems = _processor.getNumOkItemsProcessed();

    auto result = routing_context_utils::runAndValidate(
        *swRoutingCtx.getValue(), [&](RoutingContext& routingCtx) -> ProcessorResult {
            // Create an RAII object that prints each collection's shard key in the case of a
            // tassert or crash.
            stdx::unordered_map<NamespaceString, boost::optional<BSONObj>> shardKeys;
            for (auto& nss : nssList) {
                const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
                shardKeys.emplace(nss,
                                  cri.isSharded()
                                      ? boost::optional<BSONObj>(
                                            cri.getChunkManager().getShardKeyPattern().toBSON())
                                      : boost::none);
            }
            ScopedDebugInfo shardKeyDiagnostics(
                "ShardKeyDiagnostics",
                diagnostic_printers::MultipleShardKeysDiagnosticPrinter{shardKeys});

            // Call getNextBatch() and handle any target errors that occurred.
            auto batch = getNextBatchAndHandleTargetErrors(opCtx, routingCtx);

            // Prepare the RoutingContext for executing the batch.
            prepareRoutingContext(routingCtx, nssList, batch);

            // Execute the batch.
            auto batchResponse = _executor.execute(opCtx, routingCtx, batch);

            // Process the responses.
            return _processor.onWriteBatchResponse(opCtx, routingCtx, batchResponse);
        });

    // Save the stale versions from this round for the next round's metadata-refresh check.
    _prevStaleVersions = _processor.getLastRoundStaleVersions();
    _prevStaleDbVersions = _processor.getLastRoundStaleDbVersions();

    // Check if any progress was made during this round.
    bool madeProgress = _processor.getNumOkItemsProcessed() > previousNumOkItems;

    // If a write error occurred -AND- if the write command is ordered or running in a transaction,
    // call stopMakingBatches() to stop any further execution of this command and then return.
    if ((ordered || inTransaction) && _processor.getNumErrorsRecorded() > 0) {
        _batcher.stopMakingBatches();
        return RoundResult{.madeProgress = madeProgress, .metadataRefreshed = metadataRefreshed};
    }

    // Mark each op in 'opsToRetry' for re-processing.
    if (!result.opsToRetry.empty()) {
        _batcher.markOpReprocess(result.opsToRetry);
    }

    _batcher.noteSuccessfulShards(std::move(result.successfulShardSet));

    // Create a collection for each namespace in 'collsToCreate', and set 'madeProgress' to true
    // if the call to createCollections() successfully created one or more collections.
    madeProgress |= createCollections(opCtx, result.collsToCreate);

    return RoundResult{.madeProgress = madeProgress, .metadataRefreshed = metadataRefreshed};
}


StatusWith<std::unique_ptr<RoutingContext>> WriteBatchScheduler::initRoutingContext(
    OperationContext* opCtx, const std::vector<NamespaceString>& nssList) {
    LOGV2_DEBUG_OPTIONS(11536700,
                        2,
                        {logv2::LogComponent::kShardMigrationPerf},
                        "Creating RoutingContext in WriteBatchScheduler");

    try {
        // Attempt to create the relevant databases and create a RoutingContext.
        const bool checkTsBucketsNss = true;
        const bool refresh = false;
        auto routingCtx = sharding::router::createDatabasesAndGetRoutingCtx(
            opCtx, nssList, checkTsBucketsNss, refresh);

        // If '_targetEpoch' is set, verify that the collection's epoch matches '_targetEpoch'.
        if (_targetEpoch) {
            // When '_targetEpoch' is set, we should only be targeting a single namespace. In
            // general, a target epoch is only passed for $merge commands to detect concurrent
            // collection drops between rounds of inserting documents.
            tassert(11413800,
                    "Expected only one namespace when target epoch is specified",
                    nssList.size() == 1);
            const auto firstNss = nssList.front();
            const auto& cri = routingCtx->getCollectionRoutingInfo(firstNss);
            const auto& cm = cri.getChunkManager();

            // Throw a StaleEpoch exception if the collection's epoch does not match.
            uassert(StaleEpochInfo(firstNss), "Collection has been dropped", cm.hasRoutingTable());
            uassert(StaleEpochInfo(firstNss),
                    "Collection epoch has changed",
                    cm.getVersion().epoch() == _targetEpoch);
        }

        LOGV2_DEBUG_OPTIONS(11536701,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Successfully created RoutingContext in WriteBatchScheduler");

        return StatusWith(std::move(routingCtx));
    } catch (const DBException& ex) {
        // If an error occurs, log the error and return it.
        if (dynamic_cast<const ExceptionFor<ErrorCodes::StaleEpoch>*>(&ex)) {
            LOGV2_DEBUG(10896506,
                        2,
                        "Failed to create RoutingContext in WriteBatchScheduler because "
                        "collection was dropped",
                        "error"_attr = redact(ex));
        } else {
            LOGV2_WARNING(10896507,
                          "Failed to create RoutingContext in WriteBatchScheduler",
                          "error"_attr = redact(ex));
        }

        LOGV2_DEBUG_OPTIONS(11536702,
                            2,
                            {logv2::LogComponent::kShardMigrationPerf},
                            "Failed to create RoutingContext in WriteBatchScheduler",
                            "error"_attr = redact(ex));

        return ex.toStatus("Failed to create RoutingContext in WriteBatchScheduler");
    }
}

void WriteBatchScheduler::handleInitRoutingContextError(OperationContext* opCtx,
                                                        const Status& status) {
    tassert(10896508, "Unexpectedly got an OK status", !status.isOK());

    // If creating the RoutingContext failed and nothing has been processed yet, throw the error
    // as an exception.
    if (!_processor.getNumOkItemsProcessed() && !_processor.getNumErrorsRecorded()) {
        uassertStatusOK(status);
    }

    // Record an error for the remaining ops.
    recordErrorForRemainingOps(opCtx, status);
}

void WriteBatchScheduler::recordErrorForRemainingOps(OperationContext* opCtx,
                                                     const Status& status) {
    for (auto& op : _batcher.getAllRemainingOps()) {
        _processor.recordError(opCtx, op, status);
    }
    _batcher.stopMakingBatches();
}

WriteBatch WriteBatchScheduler::getNextBatchAndHandleTargetErrors(OperationContext* opCtx,
                                                                  RoutingContext& routingCtx) {
    const bool ordered = _cmdRef.getOrdered();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    auto result = _batcher.getNextBatch(opCtx, routingCtx);

    if (!result.opsWithErrors.empty()) {
        // Record the errors that occurred during the batch creation process.
        _processor.recordTargetErrors(opCtx, result);

        // If an unrecoverable error occurred, discard the batch, call stopMakingBatches() to
        // stop any further execution of this command, and then return an empty batch.
        if (ordered || inTransaction) {
            _batcher.markBatchReprocess(std::move(result.batch));
            _batcher.stopMakingBatches();
            return WriteBatch{};
        }
    }

    // Return the batch.
    return std::move(result.batch);
}

void WriteBatchScheduler::prepareRoutingContext(RoutingContext& routingCtx,
                                                const std::vector<NamespaceString>& nssList,
                                                const WriteBatch& batch) {
    auto involvedNssSet = batch.getInvolvedNamespaces();
    bool executorUsesProvidedRoutingCtx = _executor.usesProvidedRoutingContext(batch);
    for (const auto& nss : nssList) {
        // If 'nss' is not involve with 'batch', or if the executor is not going to use 'routingCtx'
        // when executing 'batch', release 'nss' from 'routingCtx'.
        if (!executorUsesProvidedRoutingCtx || !involvedNssSet.count(nss)) {
            routingCtx.release(nss);
        }
    }
}

bool WriteBatchScheduler::createCollection(OperationContext* opCtx, const NamespaceString& nss) {
    try {
        cluster::createCollectionWithRouterLoop(opCtx, nss);
        LOGV2_DEBUG(10896509, 3, "Successfully created collection", "nss"_attr = nss);
    } catch (const DBException& ex) {
        LOGV2(10896510, "Could not create collection", "error"_attr = redact(ex.toStatus()));
        return false;
    }

    return true;
}

bool WriteBatchScheduler::createCollections(OperationContext* opCtx,
                                            const CollectionsToCreate& collsToCreate) {
    bool createdCollections = false;
    if (!collsToCreate.empty()) {
        for (auto& [nss, _] : collsToCreate) {
            createdCollections |= createCollection(opCtx, nss);
        }
    }

    return createdCollections;
}

}  // namespace unified_write_executor
}  // namespace mongo
