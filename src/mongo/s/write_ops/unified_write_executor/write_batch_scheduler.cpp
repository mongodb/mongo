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
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/server_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {

void WriteBatchScheduler::run(OperationContext* opCtx) {
    // The loop below uses an exponential backoff scheme that kicks in when there are one or more
    // consecutive rounds that don't make progress. If there are too many consecutive rounds without
    // progress, this function will eventually give up and fail with an error. These variables are
    // used to implement this mechanism.
    size_t rounds = 0;
    size_t numRoundsWithoutProgress = 0;
    Backoff backoff(Seconds(1), Seconds(2));

    // Keep executing rounds until the batcher says it can't make any more batches or until an
    // unrecoverable error or exception occurs.
    while (!_batcher.isDone()) {
        // If there have been too many consecutive rounds without progress, record an error for
        // the remaining ops and break out of the loop.
        if (numRoundsWithoutProgress > kMaxRoundsWithoutProgress) {
            Status status{ErrorCodes::NoProgressMade,
                          str::stream() << "No progress was made executing write ops in after "
                                        << kMaxRoundsWithoutProgress << " rounds (" << rounds
                                        << " rounds total)"};
            recordErrorForRemainingOps(opCtx, status);
            break;
        }

        // If no progress was made during the previous round, do exponential backoff before
        // starting this round.
        if (numRoundsWithoutProgress > 0) {
            sleepFor(backoff.nextSleep());
        }

        // Execute a round.
        bool madeProgress = executeRound(opCtx);

        // Increment 'rounds', update 'numRoundsWithoutProgress', and print a message to the log.
        ++rounds;
        numRoundsWithoutProgress = !madeProgress ? numRoundsWithoutProgress + 1 : 0;
        LOGV2_DEBUG(10896504, 4, "Completed round", "rounds completed"_attr = rounds);
    }
}

bool WriteBatchScheduler::executeRound(OperationContext* opCtx) {
    const bool ordered = _cmdRef.getOrdered();
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const auto nssList = std::vector<NamespaceString>{_nssSet.begin(), _nssSet.end()};

    // If we've exceeded our memory limit, stop execution.
    if (_processor.checkBulkWriteReplyMaxSize(opCtx)) {
        _batcher.stopMakingBatches();
        return false;
    }

    // Create a RoutingContext. If creating the RoutingContext fails, handle the failure and
    // return an empty vector.
    auto swRoutingCtx = initRoutingContext(opCtx, nssList);
    if (!swRoutingCtx.isOK()) {
        handleInitRoutingContextError(opCtx, swRoutingCtx.getStatus());
        return false;
    }

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

    // Check if any progress was made during this round.
    bool madeProgress = _processor.getNumOkItemsProcessed() > previousNumOkItems;

    // If a write error occurred -AND- if the write command is ordered or running in a transaction,
    // call stopMakingBatches() to stop any further execution of this command and then return.
    if ((ordered || inTransaction) && _processor.getNumErrorsRecorded() > 0) {
        _batcher.stopMakingBatches();
        return madeProgress;
    }

    // Mark each op in 'opsToRetry' for re-processing.
    if (!result.opsToRetry.empty()) {
        _batcher.markOpReprocess(result.opsToRetry);
    }

    _batcher.noteSuccessfulShards(result.successfulShardSet);

    // Create a collection for each namespace in 'collsToCreate', and set 'madeProgress' to true
    // if the call to createCollections() successfully created one or more collections.
    madeProgress |= createCollections(opCtx, result.collsToCreate);

    return madeProgress;
}

StatusWith<std::unique_ptr<RoutingContext>> WriteBatchScheduler::initRoutingContext(
    OperationContext* opCtx, const std::vector<NamespaceString>& nssList) {
    constexpr size_t kMaxAttempts = 3u;

    // Check to make sure isCollectionlessAggregateNS() is false for all names in 'nssList'.
    for (const auto& nss : nssList) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Must use real namespaces with WriteBatchScheduler, got "
                              << nss.toStringForErrorMsg(),
                !nss.isCollectionlessAggregateNS());
    }

    size_t attempts = 0;

    for (;;) {
        ++attempts;

        try {
            // Ensure all the relevant databases exist.
            std::set<DatabaseName> dbSet;
            for (const auto& nss : nssList) {
                if (auto [it, inserted] = dbSet.insert(nss.dbName()); inserted) {
                    const auto& dbName = *it;
                    cluster::createDatabase(opCtx, dbName);
                }
            }

            // Create a RoutingContext and return it. If the RoutingContext constructor fails, an
            // exception will be thrown.
            const auto allowLocks = opCtx->inMultiDocumentTransaction() &&
                shard_role_details::getLocker(opCtx)->isLocked();


            // We should only be passing the target epoch if we're targeting one namespace. In
            // general, a target epoch is only passed for $merge commands to detect concurrent
            // collection drops between rounds of inserting documents.
            tassert(11413801, "Expected at least one nss to be targeted", nssList.size() > 0);
            tassert(11413800,
                    "Expected only one namespace when target epoch is specified",
                    !_targetEpoch || nssList.size() == 1);
            const auto firstNss = nssList.front();

            auto routingCtx = std::make_unique<RoutingContext>(
                opCtx, std::move(nssList), allowLocks, true /* checkTimeseriesBucketsNss */);

            // Throws a StaleEpoch exception if the collection has been dropped and recreated or has
            // an epoch that doesn't match that target epoch specified.
            if (_targetEpoch) {
                const auto& cri = routingCtx->getCollectionRoutingInfo(firstNss);
                const auto& cm = cri.getChunkManager();

                uassert(StaleEpochInfo(firstNss, ShardVersion{}, ShardVersion{}),
                        "Collection has been dropped",
                        cm.hasRoutingTable());
                uassert(StaleEpochInfo(firstNss, ShardVersion{}, ShardVersion{}),
                        "Collection epoch has changed",
                        cm.getVersion().epoch() == _targetEpoch);
            }

            return std::move(routingCtx);

        } catch (const DBException& ex) {
            // For NamespaceNotFound errors, we will retry a couple of times before returning
            // the error to the caller. For all other types of errors, we return the error to
            // the caller immediately.
            if (dynamic_cast<const ExceptionFor<ErrorCodes::NamespaceNotFound>*>(&ex)) {
                LOGV2_INFO(10896505,
                           "RoutingContext initialization failed due to a NamespaceNotFound error",
                           "reason"_attr = ex.reason(),
                           "attemptNumber"_attr = attempts,
                           "maxAttempts"_attr = kMaxAttempts);

                // If the maximum number of attempts has not been reached, continue and try
                // again.
                if (attempts < kMaxAttempts) {
                    continue;
                }
            } else if (dynamic_cast<const ExceptionFor<ErrorCodes::StaleEpoch>*>(&ex)) {
                LOGV2_DEBUG(10896506,
                            2,
                            "Failed to refresh RoutingContext in WriteBatchScheduler because "
                            "collection was dropped",
                            "error"_attr = redact(ex));
            } else {
                LOGV2_WARNING(10896507,
                              "Failed to refresh RoutingContext in WriteBatchScheduler",
                              "error"_attr = redact(ex));
            }
            // Return the error.
            return ex.toStatus("Failed to refresh RoutingContext in WriteBatchScheduler");
        }
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
