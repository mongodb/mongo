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

#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/s/range_deleter_service_op_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
namespace {
const auto rangeDeleterServiceDecorator = ServiceContext::declareDecoration<RangeDeleterService>();
}

const ReplicaSetAwareServiceRegistry::Registerer<RangeDeleterService>
    rangeDeleterServiceRegistryRegisterer("RangeDeleterService");

RangeDeleterService* RangeDeleterService::get(ServiceContext* serviceContext) {
    return &rangeDeleterServiceDecorator(serviceContext);
}

RangeDeleterService* RangeDeleterService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void RangeDeleterService::onStepUpComplete(OperationContext* opCtx, long long term) {
    if (!feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
        return;
    }

    auto lock = _acquireMutexUnconditionally();
    dassert(_state.load() == kDown, "Service expected to be down before stepping up");

    _state.store(kInitializing);

    if (_executor) {
        // Join previously shutted down executor before reinstantiating it
        _executor->join();
        _executor.reset();

        // Reset potential in-memory state referring a previous term
        _rangeDeletionTasks.clear();
    } else {
        // Initializing the op observer, only executed once at the first step-up
        auto opObserverRegistry =
            checked_cast<OpObserverRegistry*>(opCtx->getServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(std::make_unique<RangeDeleterServiceOpObserver>());
    }

    const std::string kExecName("RangeDeleterServiceExecutor");
    auto net = executor::makeNetworkInterface(kExecName);
    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
    auto taskExecutor =
        std::make_shared<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    _executor = std::move(taskExecutor);
    _executor->startup();

    _recoverRangeDeletionsOnStepUp();
}

void RangeDeleterService::_recoverRangeDeletionsOnStepUp() {

    if (disableResumableRangeDeleter.load()) {
        _state.store(kDown);
        return;
    }

    // TODO SERVER-68348 Asynchronously register tasks on the range deleter service on step-up
    _state.store(kUp);
}

void RangeDeleterService::onStepDown() {
    if (!feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCV()) {
        return;
    }

    auto lock = _acquireMutexUnconditionally();
    dassert(_state.load() != kDown, "Service expected to be initializing/up before stepping down");

    _executor->shutdown();

    _state.store(kDown);
}

BSONObj RangeDeleterService::dumpState() {
    auto lock = _acquireMutexUnconditionally();

    BSONObjBuilder builder;
    for (const auto& [collUUID, chunkRanges] : _rangeDeletionTasks) {
        BSONArrayBuilder subBuilder(builder.subarrayStart(collUUID.toString()));
        for (const auto& chunkRange : chunkRanges) {
            subBuilder.append(chunkRange->toBSON());
        }
    }
    return builder.obj();
}

long long RangeDeleterService::totalNumOfRegisteredTasks() {
    auto lock = _acquireMutexUnconditionally();

    long long counter = 0;
    for (const auto& [collUUID, ranges] : _rangeDeletionTasks) {
        counter += ranges.size();
    }
    return counter;
}

SharedSemiFuture<void> RangeDeleterService::registerTask(
    const RangeDeletionTask& rdt, SemiFuture<void>&& waitForActiveQueriesToComplete) {

    if (disableResumableRangeDeleter.load()) {
        return SemiFuture<void>::makeReady(
                   Status(ErrorCodes::ResumableRangeDeleterDisabled,
                          "Not submitting any range deletion task because the "
                          "disableResumableRangeDeleter server parameter is set to true"))
            .share();
    }

    // Block the scheduling of the task while populating internal data structures
    SharedPromise<void> blockUntilRegistered;

    auto chainCompletionFuture =
        blockUntilRegistered.getFuture()
            .semi()
            .thenRunOn(_executor)
            .onError([serializedTask = rdt.toBSON()](Status errStatus) {
                // The above futures can only fail with those specific codes (futures notifying
                // the end of ongoing queries on a range will never be set to an error):
                // - 67635: the task was already previously scheduled
                // - BrokenPromise: the executor is shutting down
                // - Cancellation error: the node is shutting down or a stepdown happened
                if (errStatus.code() != 67635 && errStatus != ErrorCodes::BrokenPromise &&
                    !ErrorCodes::isCancellationError(errStatus)) {
                    LOGV2_ERROR(6784800,
                                "Range deletion scheduling failed with unexpected error",
                                "error"_attr = errStatus,
                                "rangeDeletion"_attr = serializedTask);
                }
                return errStatus;
            })
            .then([waitForOngoingQueries = std::move(waitForActiveQueriesToComplete).share()]() {
                return waitForOngoingQueries;
            })
            .then([this, when = rdt.getWhenToClean()]() {
                // Step 2: schedule wait for secondaries orphans cleanup delay
                const auto delayForActiveQueriesOnSecondariesToComplete =
                    when == CleanWhenEnum::kDelayed ? Seconds(orphanCleanupDelaySecs.load())
                                                    : Seconds(0);

                return sleepUntil(_executor,
                                  _executor->now() + delayForActiveQueriesOnSecondariesToComplete)
                    .share();
            })
            .then([this, collUuid = rdt.getCollectionUuid(), range = rdt.getRange()]() {
                // Step 3: perform the actual range deletion
                // TODO

                // Deregister the task
                deregisterTask(collUuid, range);
            })
            // IMPORTANT: no continuation should be added to this chain after this point
            // in order to make sure range deletions order is preserved.
            .semi()
            .share();

    auto [taskCompletionFuture, inserted] = [&]() -> std::pair<SharedSemiFuture<void>, bool> {
        auto lock = _acquireMutexFailIfServiceNotUp();
        auto [registeredTask, inserted] = _rangeDeletionTasks[rdt.getCollectionUuid()].insert(
            std::make_shared<RangeDeletion>(RangeDeletion(rdt, chainCompletionFuture)));
        auto retFuture = static_cast<RangeDeletion*>(registeredTask->get())->getCompletionFuture();
        return {retFuture, inserted};
    }();

    if (inserted) {
        // The range deletion task has been registered, so the chain execution can be unblocked
        blockUntilRegistered.setFrom(Status::OK());
    } else {
        // Tried to register a duplicate range deletion task: invalidate the chain
        auto errStatus =
            Status(ErrorCodes::Error(67635), "Not scheduling duplicated range deletion");
        LOGV2_WARNING(6804200,
                      "Tried to register duplicate range deletion task. This results in a no-op.",
                      "collectionUUID"_attr = rdt.getCollectionUuid(),
                      "range"_attr = rdt.getRange());
        blockUntilRegistered.setFrom(errStatus);
    }

    return taskCompletionFuture;
}

void RangeDeleterService::deregisterTask(const UUID& collUUID, const ChunkRange& range) {
    auto lock = _acquireMutexFailIfServiceNotUp();
    _rangeDeletionTasks[collUUID].erase(std::make_shared<ChunkRange>(range));
}

int RangeDeleterService::getNumRangeDeletionTasksForCollection(const UUID& collectionUUID) {
    auto lock = _acquireMutexFailIfServiceNotUp();
    auto tasksSet = _rangeDeletionTasks.find(collectionUUID);
    if (tasksSet == _rangeDeletionTasks.end()) {
        return 0;
    }
    return tasksSet->second.size();
}

SharedSemiFuture<void> RangeDeleterService::getOverlappingRangeDeletionsFuture(
    const UUID& collectionUUID, const ChunkRange& range) {

    if (disableResumableRangeDeleter.load()) {
        return SemiFuture<void>::makeReady(
                   Status(ErrorCodes::ResumableRangeDeleterDisabled,
                          "Not submitting any range deletion task because the "
                          "disableResumableRangeDeleter server parameter is set to true"))
            .share();
    }

    auto lock = _acquireMutexFailIfServiceNotUp();

    auto mapEntry = _rangeDeletionTasks.find(collectionUUID);
    if (mapEntry == _rangeDeletionTasks.end() || mapEntry->second.size() == 0) {
        // No tasks scheduled for the specified collection
        return SemiFuture<void>::makeReady().share();
    }

    std::vector<ExecutorFuture<void>> overlappingRangeDeletionsFutures;

    auto& rangeDeletions = mapEntry->second;
    const auto rangeSharedPtr = std::make_shared<ChunkRange>(range);
    auto forwardIt = rangeDeletions.lower_bound(rangeSharedPtr);
    if (forwardIt != rangeDeletions.begin()) {
        forwardIt--;
    }

    while (forwardIt != rangeDeletions.end() && forwardIt->get()->overlapWith(range)) {
        auto future = static_cast<RangeDeletion*>(forwardIt->get())->getCompletionFuture();
        // Scheduling wait on the current executor so that it gets invalidated on step-down
        overlappingRangeDeletionsFutures.push_back(future.thenRunOn(_executor));
        forwardIt++;
    }

    if (overlappingRangeDeletionsFutures.size() == 0) {
        return SemiFuture<void>::makeReady().share();
    }
    return whenAllSucceed(std::move(overlappingRangeDeletionsFutures)).share();
}

}  // namespace mongo
