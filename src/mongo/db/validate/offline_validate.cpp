// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/offline_validate.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/synchronized_value.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo ::offline_validate {
namespace {

struct OfflineValidateResults {
    bool allValidationComplete = true;
    bool allResultsValid = true;
};

/**
 * Runs collection_validation::validate() as its own operation, so that a validation slower than
 * 'slowms' reports the same "Slow query" log line as the 'validate' command. 'validateOptions' are
 * the options the validation was configured with, reported as part of the command.
 */
Status validateAndLogOperation(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& validateOptions,
                               const collection_validation::ValidationOptions& options,
                               ValidateResults& results) {
    CurOp curOp;
    curOp.push(opCtx);
    {
        BSONObjBuilder cmdObj;
        cmdObj.append("validate", nss.coll());
        cmdObj.appendElementsUnique(validateOptions);
        ClientLock lk(opCtx->getClient());
        // The registered command lets the log line redact and report the command as it does for
        // the 'validate' command.
        curOp.setGenericOpRequestDetails(
            lk, nss, CommandHelpers::findCommand(opCtx, "validate"), cmdObj.obj(), dbMsg);
    }
    curOp.ensureStarted();
    ON_BLOCK_EXIT([&] {
        try {
            curOp.completeAndLogOperation(
                {logv2::LogComponent::kCommand, toLogService(opCtx->getService())},
                DatabaseProfileSettings::get(opCtx->getServiceContext())
                    .getDatabaseProfileSettings(nss.dbName())
                    .filter);
        } catch (const DBException& e) {
            LOGV2(13618301, "Unable to log offline validation operation", "error"_attr = e);
        }
    });
    return collection_validation::validate(opCtx, nss, options, results);
}

// Performs collection validation for a singular collection.
// Returns if the collection is valid, or the validation didn't complete correctly.
StatusWith<bool> offlineValidateCollection(OperationContext* opCtx,
                                           NamespaceString nss,
                                           bool skipAtClusterTime = false) {
    const auto collectionValidateOptionsParam =
        ServerParameterSet::getNodeParameterSet()->get<CollectionValidateOptionsServerParameter>(
            "collectionValidateOptions");
    const auto validateOptions = collectionValidateOptionsParam->_data.getOptions();

    const auto parsedOptions = !validateOptions.isEmpty()
        ? collection_validation::parseValidateOptions(
              opCtx, nss, validateOptions, skipAtClusterTime, /*enableSizeStats=*/true)
        : collection_validation::ValidationOptions{
              .validateMode = collection_validation::ValidateMode::kForegroundFull,
              .repairMode = collection_validation::RepairMode::kNone,
              .logDiagnostics = false,
              .targetRecordsPerRecordStoreSlice =
                  collection_validation::getTargetRecordsPerRecordStoreSlice()};
    if (parsedOptions.repairMode != collection_validation::RepairMode::kNone) {
        return Status{ErrorCodes::InvalidOptions,
                      "Repair is not allowed in offline validation mode"};
    }
    ValidateResults validateResults;
    try {
        Status status =
            validateAndLogOperation(opCtx, nss, validateOptions, parsedOptions, validateResults);

        if (!status.isOK()) {
            LOGV2_ERROR(11790200,
                        "Collection validation failed to complete, see logs for more details",
                        "nss"_attr = nss.toStringForErrorMsg(),
                        "error"_attr = status.toString());
            return status;
        }
    } catch (const DBException& e) {
        // When validating at 'atClusterTime', a collection present in the latest catalog may not
        // have existed at that timestamp because it was created or renamed into place afterwards;
        // validate() throws NamespaceNotFound. That is a valid catalog state rather than
        // corruption, so skip the collection and continue validating the rest.
        if (e.code() == ErrorCodes::NamespaceNotFound && parsedOptions.readTimestamp) {
            if (!gValidateCollectionName.empty()) {
                // If validating a single collection, return non-OK status to indicate that the
                // collection was not validated and return early.
                LOGV2_ERROR(
                    11790202,
                    "Single collection validation failed to complete, see logs for more details",
                    "nss"_attr = nss.toStringForErrorMsg(),
                    "error"_attr = e.toString());
                return e.toStatus();
            }
            LOGV2(11790100,
                  "Skipping validation of collection because it did not exist at atClusterTime",
                  "nss"_attr = nss.toStringForErrorMsg(),
                  "atClusterTime"_attr = *parsedOptions.readTimestamp);
            return true;
        }
        LOGV2_ERROR(11790201,
                    "Collection validation failed to complete, see logs for more details",
                    "nss"_attr = nss.toStringForErrorMsg(),
                    "error"_attr = e.toString());
        return e.toStatus();
    }

    BSONObjBuilder results;
    validateResults.appendToResultObj(&results, /*debugging=*/false);
    LOGV2_OPTIONS(9437301,
                  {logv2::LogTruncation::Disabled},
                  "Offline validation result",
                  "results"_attr = results.done());
    return validateResults.isValid();
}

OfflineValidateResults offlineValidateParallel(OperationContext* opCtx,
                                               boost::optional<Lock::GlobalWrite> globalWriteLock,
                                               size_t maxThreadCount) {
    invariant(!storageGlobalParams.queryableBackupMode);
    // Must have global write lock before beginning offlineValidateParallel
    invariant(globalWriteLock && globalWriteLock->isLocked());

    // Must only be called before the server accepts network connections. The lighter per-worker
    // lock mode (IX at global, X at collection) relies on there being no concurrent writers or
    // DDL operations — a guarantee that only holds at startup.
    {
        auto* tlm = opCtx->getServiceContext()->getTransportLayerManager();
        fassert(12759001, !tlm || !tlm->hasActiveSessions());
    }

    auto& serviceLifecycle = rss::ReplicatedStorageService::get(opCtx).getServiceLifecycle();
    serviceLifecycle.initializeStateRequiredForOfflineValidation(opCtx);
    const auto dbNames = std::invoke([opCtx]() -> std::vector<DatabaseName> {
        if (gValidateDbName.empty()) {
            return CollectionCatalog::get(opCtx)->getAllDbNames();
        }
        return std::vector{DatabaseNameUtil::deserialize(
            /*tenantId=*/boost::none,
            gValidateDbName.data(),
            SerializationContext(SerializationContext::Source::Catalog))};
    });
    auto databaseHolder = DatabaseHolder::get(opCtx);
    for (const auto& dbName : dbNames) {
        databaseHolder->openDb(opCtx, dbName);
    }

    globalWriteLock.reset();
    Lock::GlobalLock catalogLock(opCtx, MODE_IS);
    const auto catalog = CollectionCatalog::get(opCtx);

    // Collect every namespace to validate along with its data size, so that the largest
    // collections can be dispatched first. Scheduling longest-processing-time-first keeps a
    // large collection from being picked up late and running as a long tail.
    std::vector<std::pair<long long, NamespaceString>> collectionsBySize;
    for (const auto& dbName : dbNames) {
        for (const auto& coll : catalog->range(dbName)) {
            collectionsBySize.emplace_back(coll->dataSize(opCtx), NamespaceString{coll->ns()});
        }
    }

    // Sort descending by data size, breaking ties by namespace to keep dispatch order
    // deterministic.
    std::sort(collectionsBySize.begin(), collectionsBySize.end(), std::greater<>());

    synchronized_value<OfflineValidateResults> results{OfflineValidateResults{
        .allValidationComplete = true,
        .allResultsValid = true,
    }};

    if (collectionsBySize.empty()) {
        return results.get();
    }

    const size_t numCores = ProcessInfo::getNumAvailableCores();
    maxThreadCount = maxThreadCount == 0 ? numCores : std::min(maxThreadCount, numCores);
    // Do not exceed the number of collections
    maxThreadCount = std::min(maxThreadCount, collectionsBySize.size());
    SingleProducerMultiConsumerQueue<NamespaceString> queue{
        {.maxQueueDepth = maxThreadCount * 4ULL}};

    auto threadPool = ThreadPool::make({
        .poolName = "ParallelOfflineValidate",
        .threadNamePrefix = "NamespaceValidationWorker",
        .maxThreads = maxThreadCount,
        .onCreateThread =
            [](const auto& threadName) {
                Client::initThread(threadName, getGlobalServiceContext()->getService());
            },
    });
    threadPool->startup();

    // Create worker threads
    for (size_t i = 0; i < maxThreadCount; ++i) {
        threadPool->schedule([&queue, &results](Status status) {
            if (!status.isOK()) {
                results->allValidationComplete = false;
                return;
            }
            auto opCtx = cc().makeOperationContext();

            while (true) {
                try {
                    // Pull namespaces off the queue to run validation against.
                    const auto nss = queue.pop(opCtx.get());
                    auto swValidateResult =
                        offlineValidateCollection(opCtx.get(), nss, !nss.isReplicated());

                    // Once complete, synchronize the results.  This could be an atomic, but the
                    // contention window for the lock should be quite small so it will be
                    // synchronized value using a mutex.
                    auto resultsGuard = results.synchronize();
                    if (swValidateResult.isOK()) {
                        resultsGuard->allResultsValid =
                            resultsGuard->allResultsValid && swValidateResult.getValue();
                    } else {
                        resultsGuard->allValidationComplete = false;
                    }
                } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueConsumed>&) {
                    // queue exhausted, end worker thread
                    break;
                } catch (const DBException& e) {
                    LOGV2_ERROR(12759002,
                                "An error occurred running validation",
                                "error"_attr = e.toString());
                    results->allValidationComplete = false;
                }
            }
        });
    }

    // The queue is FIFO and bounded, so pushing in sorted order is enough to control the order
    // work is picked up in; `maxQueueDepth` only throttles how far ahead this producer may run.
    for (auto&& [_, nss] : collectionsBySize) {
        queue.push(std::move(nss));
    }
    queue.closeProducerEnd();

    // Synchronize the thread pool and wait for all workers to finish.
    threadPool->waitForIdle();
    threadPool->shutdown();
    threadPool->join();
    return results.get();
}

// Performs collection validation for all collections or one collection in a database.
// Returns if all collections complete validation and if all collections are valid.
OfflineValidateResults offlineValidateDb(OperationContext* opCtx, DatabaseName dbName) {
    OfflineValidateResults offlineValidateResults;
    if (!gValidateCollectionName.empty()) {
        NamespaceString userNss = NamespaceStringUtil::deserialize(dbName, gValidateCollectionName);
        if (auto swValidateResult = offlineValidateCollection(opCtx, userNss);
            swValidateResult.isOK()) {
            offlineValidateResults.allResultsValid = swValidateResult.getValue();
        } else {
            offlineValidateResults.allValidationComplete = false;
        }
    } else {
        for (const auto& nss :
             CollectionCatalog::get(opCtx)->getAllCollectionNamesFromDb(opCtx, dbName)) {
            opCtx->checkForInterrupt();

            if (auto swValidateResult = offlineValidateCollection(opCtx, nss, !nss.isReplicated());
                swValidateResult.isOK()) {
                offlineValidateResults.allResultsValid =
                    offlineValidateResults.allResultsValid && swValidateResult.getValue();
            } else {
                offlineValidateResults.allValidationComplete = false;
            }
        }
    }
    return offlineValidateResults;
}

// Performs collection validation for all collections in one or all databases.
// Returns if all databases complete validation and if all databases are valid.
OfflineValidateResults offlineValidate(OperationContext* opCtx) {
    invariant(!storageGlobalParams.queryableBackupMode);

    auto& serviceLifecycle = rss::ReplicatedStorageService::get(opCtx).getServiceLifecycle();
    serviceLifecycle.initializeStateRequiredForOfflineValidation(opCtx);

    OfflineValidateResults offlineValidateResults;

    auto databaseHolder = DatabaseHolder::get(opCtx);
    const auto dbNames = std::invoke([opCtx]() -> std::vector<DatabaseName> {
        if (gValidateDbName.empty()) {
            return CollectionCatalog::get(opCtx)->getAllDbNames();
        }
        const boost::optional<TenantId>& tenantId = boost::none;
        return std::vector{DatabaseNameUtil::deserialize(
            tenantId,
            gValidateDbName.data(),
            SerializationContext(SerializationContext::Source::Catalog))};
    });
    for (const auto& dbName : dbNames) {
        databaseHolder->openDb(opCtx, dbName);
    }
    for (const auto& dbName : dbNames) {
        const auto [isComplete, isValid] = offlineValidateDb(opCtx, dbName);
        offlineValidateResults.allValidationComplete =
            offlineValidateResults.allValidationComplete && isComplete;
        offlineValidateResults.allResultsValid = offlineValidateResults.allResultsValid && isValid;
    }
    return offlineValidateResults;
}

}  // namespace

void runOfflineValidation(OperationContext* opCtx,
                          boost::optional<Lock::GlobalWrite>&& globalWriteLock) {
    // If the feature flag is enabled and a collection is not specified, run concurrent
    // validations across the database instance.
    const bool runParallel =
        gFeatureFlagParallelCollectionValidation.isEnabled() && gValidateCollectionName.empty();

    const auto offlineValidateResults = runParallel
        ? offlineValidateParallel(
              opCtx, std::move(globalWriteLock), gValidateParallelMaxConcurrentNamespaces.load())
        : offlineValidate(opCtx);

    if (offlineValidateResults.allResultsValid) {
        LOGV2(9437303, "Offline validation detected no issues");
    } else {
        LOGV2(9437304, "Offline validation found issues in some collections, see logs for details");
    }

    if (!offlineValidateResults.allValidationComplete) {
        uassertStatusOK({ErrorCodes::OfflineValidationFailedToComplete,
                         "Offline validation didn't complete for some collections"});
    }
}

}  // namespace mongo::offline_validate
