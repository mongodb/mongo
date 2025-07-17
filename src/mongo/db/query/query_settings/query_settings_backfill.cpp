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

#include "mongo/db/query/query_settings/query_settings_backfill.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::query_settings {

MONGO_FAIL_POINT_DEFINE(throwBeforeSchedulingBackfillTask);

using query_shape::QueryShapeHash;

namespace {
std::vector<QueryShapeHash> removeNonPersistedHashes(
    std::vector<QueryShapeHash> hashes, std::vector<mongo::write_ops::WriteError> writeErrors) {
    stdx::unordered_set<QueryShapeHash> failedHashes;
    for (auto&& error : writeErrors) {
        if (error.getStatus().code() != ErrorCodes::DuplicateKey) {
            failedHashes.insert(hashes[error.getIndex()]);
        }
    }
    hashes.erase(std::remove_if(hashes.begin(),
                                hashes.end(),
                                [&](auto&& hash) { return failedHashes.count(hash); }),
                 hashes.end());
    LOGV2_DEBUG(10377203,
                2,
                "not all configurations were succesfully backfilled",
                "success"_attr = hashes,
                "failed"_attr = failedHashes);
    return hashes;
}

std::vector<QueryShapeHash> handleInsertReply(
    std::vector<QueryShapeHash> hashes,
    StatusWith<async_rpc::AsyncRPCResponse<write_ops::InsertCommandReply>> reply) {
    // All inserts succeded, report all the hashes.
    auto&& status = reply.getStatus();
    if (status.isOK()) {
        LOGV2_DEBUG(10377204,
                    2,
                    "succesfully persisted all representative queries",
                    "hashes"_attr = hashes);
        return hashes;
    }

    // No insert succeded.
    if (auto unpackedStatus = async_rpc::unpackRPCStatusIgnoringWriteErrors(status);
        !unpackedStatus.isOK()) {
        LOGV2_WARNING(10377200,
                      "encountered unrecoverable error while inserting representative queries",
                      "error"_attr = redact(unpackedStatus),
                      "hashes"_attr = hashes);
        return {};
    }

    // Not all the representative queries were inserted. Remove the hashes which failed
    // with non-duplicate errors.
    auto&& errorInfo = status.extraInfo<AsyncRPCErrorInfo>();
    tassert(10377201, "expected remote error", errorInfo->isRemote());
    auto writeErrors =
        InsertOp::parseResponse(errorInfo->asRemote().getResponseObj()).getWriteErrors();
    tassert(10377202, "expected writeErrors to be present", writeErrors.has_value());
    return removeNonPersistedHashes(std::move(hashes), std::move(*writeErrors));
};

write_ops::InsertCommandRequest makeInsertCommandRequest(auto documents) {
    write_ops::InsertCommandRequest insertOp{
        NamespaceString::kQueryShapeRepresentativeQueriesNamespace};
    insertOp.setDocuments(std::move(documents));
    insertOp.setOrdered(false);
    insertOp.setWriteConcern(defaultMajorityWriteConcern());
    return insertOp;
}

ExecutorFuture<std::vector<QueryShapeHash>> dispatchBatchedInsert(
    OperationContext* opCtx,
    std::vector<BSONObj> documents,
    std::vector<QueryShapeHash> hashes,
    std::unique_ptr<async_rpc::Targeter> targeter,
    std::shared_ptr<executor::TaskExecutor> executor) {
    auto request = makeInsertCommandRequest(std::move(documents));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<write_ops::InsertCommandRequest>>(
        executor, CancellationToken::uncancelable(), std::move(request));
    return async_rpc::sendCommand<write_ops::InsertCommandRequest>(opts, opCtx, std::move(targeter))
        .onCompletion([hashes = std::move(hashes)](auto reply) {
            // Map the insert reply to a list of inserted hashes.
            return handleInsertReply(std::move(hashes), std::move(reply));
        });
}

std::vector<QueryShapeHash> flattenVector(std::vector<std::vector<QueryShapeHash>> vecOfVecs) {
    // Re-use one of the vectors as the final buffer.
    std::vector<QueryShapeHash> buffer = std::move(vecOfVecs.back());
    vecOfVecs.pop_back();

    // Re-size the vector such that it can fit all the elems.
    const std::size_t bufferSize = std::accumulate(
        vecOfVecs.begin(), vecOfVecs.end(), buffer.size(), [](std::size_t acc, auto&& vec) {
            return acc + vec.size();
        });
    buffer.reserve(bufferSize);

    // Move all the elements to the end buffer.
    for (auto&& vec : vecOfVecs) {
        for (auto&& elem : vec) {
            buffer.push_back(std::move(elem));
        }
    }
    return buffer;
}
}  // namespace

ExecutorFuture<std::vector<query_shape::QueryShapeHash>> insertRepresentativeQueriesToCollection(
    OperationContext* opCtx,
    std::vector<QueryShapeRepresentativeQuery> representativeQueries,
    MakeRepresentativeQueryTargeterFn makeRepresentativeQueryTargeterFn,
    std::shared_ptr<executor::TaskExecutor> executor) {
    // Split the 'representativeQueries' into 'BSONObjMaxUserSize' batches, dispatch the insert
    // commands and store all the future results into 'futures'.
    int32_t batchSizeBytes = 0;
    std::vector<QueryShapeHash> hashes;
    std::vector<BSONObj> documents;
    std::vector<ExecutorFuture<std::vector<QueryShapeHash>>> futures;
    for (auto&& representativeQuery : representativeQueries) {
        auto document = representativeQuery.toBSON();

        // Create & dispatch the insert command if the batch is greater than 'BSONObjMaxUserSize'.
        if (batchSizeBytes > BSONObjMaxUserSize - document.objsize()) {
            futures.push_back(dispatchBatchedInsert(opCtx,
                                                    std::move(documents),
                                                    std::move(hashes),
                                                    makeRepresentativeQueryTargeterFn(opCtx),
                                                    executor));
            // Re-initialize all buffers & counters.
            batchSizeBytes = 0;
            std::vector<BSONObj>{}.swap(documents);
            std::vector<QueryShapeHash>{}.swap(hashes);
        }

        // Keep track of the hash and increment the statement id.
        batchSizeBytes += document.objsize();
        documents.push_back(std::move(document));
        hashes.push_back(std::move(representativeQuery.get_id()));
    }
    if (documents.size() > 0) {
        futures.push_back(dispatchBatchedInsert(opCtx,
                                                std::move(documents),
                                                std::move(hashes),
                                                makeRepresentativeQueryTargeterFn(opCtx),
                                                executor));
    }

    // Combine all the futures to return a single flattened vector containing all the backfilled
    // query shape hashes.
    return whenAllSucceed(std::move(futures)).thenRunOn(executor).then(flattenVector);
}

BackfillCoordinator::BackfillCoordinator(OnCompletionHook onCompletionHook)
    : _state(std::make_unique<BackfillCoordinator::State>()),
      _onCompletionHook(std::move(onCompletionHook)) {}

bool BackfillCoordinator::shouldBackfill(OperationContext* opCtx, bool hasRepresentativeQuery) {
    // Nothing to do if the representative query is already present.
    if (hasRepresentativeQuery) {
        return false;
    }

    // We shouldn't attempt the backfill if it's not enabled.
    return feature_flags::gFeatureFlagPQSBackfill.isEnabledUseLatestFCVWhenUninitialized(
        VersionContext::getDecoration(opCtx),
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

void BackfillCoordinator::markForBackfillAndScheduleIfNeeded(
    OperationContext* opCtx,
    query_shape::QueryShapeHash queryShapeHash,
    QueryInstance queryInstance) try {
    stdx::lock_guard lock{_mutex};
    constexpr auto onTaskCompletion = [](Status status) {
        LOGV2_DEBUG(10493705,
                    2,
                    "Finished executing the query settings representative query backfill operation",
                    "status"_attr = status);
    };

    // Early exit if the shape was already recorded.
    auto it = _state->buffer.find(queryShapeHash);
    if (it != _state->buffer.end()) {
        return;
    }

    // Check if adding the recorded query would exceeed the buffer size threshold. Schedule an
    // immediately executing backfill operation to consume the existing buffer if that's the case.
    auto memoryLimitBytes = internalQuerySettingsBackfillMemoryLimitBytes.load();
    const std::size_t itemSize = sizeof(queryShapeHash) + queryInstance.objsize();
    if (itemSize >= memoryLimitBytes - _state->memoryUsedBytes) {
        auto prevState = std::exchange(_state, std::make_unique<State>());
        _state->taskScheduled = true;  // The original task is still scheduled.
        auto executor = makeExecutor(opCtx);
        ExecutorFuture<void>{executor}
            .then(unique_function<ExecutorFuture<void>(void)>{
                [this, executor, state = std::move(prevState)] {
                    return execute(std::move(state->buffer),
                                   std::move(state->cancellationSource),
                                   std::move(executor));
                }})
            .getAsync(onTaskCompletion);
    }

    // Add the hash <-> query pair to the buffer.
    LOGV2_DEBUG(10493701,
                2,
                "Adding the representative query to the query settings backfill buffer",
                "queryShapeHash"_attr = queryShapeHash,
                "representativeQuery"_attr = queryInstance);
    _state->buffer.emplace_hint(it, queryShapeHash, std::move(queryInstance));
    _state->memoryUsedBytes += itemSize;

    if (MONGO_unlikely(throwBeforeSchedulingBackfillTask.shouldFail())) {
        uasserted(ErrorCodes::UnknownError, "test exception while recording");
    }

    // Don't schedule a new backfill operation if there's already one in-flight.
    if (_state->taskScheduled) {
        LOGV2_DEBUG(10493702,
                    2,
                    "Skipped scheduling a new query settings representative query backfill "
                    "operation as there is one "
                    "already in-flight.");
        return;
    }

    // Schedule a delayed task to consume the buffer & execute the procedure.
    const auto duration =
        duration_cast<Milliseconds>(Seconds(internalQuerySettingsBackfillDelaySeconds.load()));
    LOGV2_DEBUG(10493703,
                2,
                "Scheduling a delayed query settings representative query backfill task",
                "duration"_attr = duration.toBSON());
    auto executor = makeExecutor(opCtx);
    executor->sleepFor(duration, _state->cancellationSource.token())
        .onError([this](Status status) {
            LOGV2_WARNING(10493704,
                          "Encountered an error while waiting for the query settings "
                          "representative query backfill operation to start",
                          "status"_attr = status);
            cancel();
            uassertStatusOK(status);
        })
        .then([this, executor] {
            auto state = consume();
            return execute(std::move(state->buffer),
                           std::move(state->cancellationSource),
                           std::move(executor));
        })
        .getAsync(onTaskCompletion);
    _state->taskScheduled = true;
} catch (const DBException& ex) {
    LOGV2_WARNING(10493706,
                  "Encountered an error while scheduling the query settings representative query "
                  "backfill operation",
                  "status"_attr = ex.toStatus());
    cancel();
}

ExecutorFuture<void> BackfillCoordinator::execute(
    RepresentativeQueryMap buffer,
    CancellationSource cancellationSource,
    std::shared_ptr<executor::TaskExecutor> executor) {
    ServiceContext::UniqueClient client =
        getGlobalServiceContext()->getService()->makeClient("QuerySettingsBackfillManager");
    auto opCtxHolder = client->makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    const boost::optional<TenantId> tenantId = boost::none;
    auto&& service = QuerySettingsService::get(opCtx);
    auto [queryShapeConfigurations, clusterParameterTime] =
        service.getAllQueryShapeConfigurations(tenantId);

    // Construct the query shape representative query array. Avoid copying over an entry if the
    // corresponding query shape configuration was removed in the meantime.
    std::vector<QueryShapeRepresentativeQuery> representativeQueries;
    representativeQueries.reserve(buffer.size());
    for (auto&& [queryShapeHash, queryInstance] : buffer) {
        auto it = std::find_if(queryShapeConfigurations.begin(),
                               queryShapeConfigurations.end(),
                               [&](const QueryShapeConfiguration& config) {
                                   return config.getQueryShapeHash() == queryShapeHash;
                               });
        if (it == queryShapeConfigurations.end()) {
            continue;
        }
        representativeQueries.emplace_back(
            std::move(queryShapeHash), std::move(queryInstance), clusterParameterTime);
    }

    // Early exit if there are no representative queries left to insert after the cleanup step.
    if (representativeQueries.size() == 0) {
        return ExecutorFuture<void>(std::move(executor));
    }

    // Insert the representative queries and then invoke the '_onCompletionHook' callback on
    // completion. Transfer the ownership of 'client' and 'opCtxHolder' so they outlive the
    // insertRepresentativeQueriesToCollection() operation.
    return insertRepresentativeQueriesToCollection(
               opCtx, std::move(representativeQueries), std::move(executor))
        .then([this,
               clusterParameterTime,
               tenantId,
               client = std::move(client),
               opCtxHolder = std::move(opCtxHolder)](std::vector<QueryShapeHash> hashes) {
            LOGV2_WARNING(10493707,
                          "Succesfully inserted the backfilled representative queries",
                          "hashes"_attr = hashes);
            _onCompletionHook(std::move(hashes), clusterParameterTime, tenantId);
        });
}

ExecutorFuture<std::vector<query_shape::QueryShapeHash>>
BackfillCoordinator::insertRepresentativeQueriesToCollection(
    OperationContext* opCtx,
    std::vector<QueryShapeRepresentativeQuery> representativeQueries,
    std::shared_ptr<executor::TaskExecutor> executor) {
    return mongo::query_settings::insertRepresentativeQueriesToCollection(
        opCtx,
        std::move(representativeQueries),
        [this](OperationContext* opCtx) { return makeTargeter(opCtx); },
        std::move(executor));
}

void BackfillCoordinator::cancel() {
    consume()->cancellationSource.cancel();
}

std::unique_ptr<BackfillCoordinator::State> BackfillCoordinator::consume() {
    auto newState = std::make_unique<BackfillCoordinator::State>();
    stdx::lock_guard lk{_mutex};
    return std::exchange(_state, std::move(newState));
}

}  // namespace mongo::query_settings
