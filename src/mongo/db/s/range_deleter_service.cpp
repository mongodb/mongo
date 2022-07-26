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
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingRangeDeleter

namespace mongo {
SharedSemiFuture<void> RangeDeleterService::registerTask(
    const RangeDeletionTask& rdt, SemiFuture<void>&& waitForActiveQueriesToComplete) {

    // Block the scheduling of the task while populating internal data structures
    SharedPromise<void> blockUntilRegistered;

    std::vector<ExecutorFuture<void>> initialFutures;
    initialFutures.push_back(blockUntilRegistered.getFuture().semi().thenRunOn(_executor));
    initialFutures.push_back(std::move(waitForActiveQueriesToComplete).thenRunOn(_executor));

    auto chainCompletionFuture =
        // Step 1: wait for the task to be registered on the service and for the draining of
        // ongoing queries that are retaining the orphaned range
        whenAllSucceed(std::move(initialFutures))
            .thenRunOn(_executor)
            .onError([&](Status s) {
                // Invalidate the chain if a task for this range had already been registered.
                // The above futures can only fail with this specific error (futures notifying
                // the end of ongoing queries on a range will never be set to an error)
                invariant(s.code() == 67635);
                return s;
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
        stdx::lock_guard<Latch> lg(_mutex);
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
    stdx::lock_guard<Latch> lg(_mutex);
    _rangeDeletionTasks[collUUID].erase(std::make_shared<ChunkRange>(range));
}

int RangeDeleterService::getNumRangeDeletionTasksForCollection(const UUID& collectionUUID) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto tasksSet = _rangeDeletionTasks.find(collectionUUID);
    if (tasksSet == _rangeDeletionTasks.end()) {
        return 0;
    }
    return tasksSet->second.size();
}
}  // namespace mongo
