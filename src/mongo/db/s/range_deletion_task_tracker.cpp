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

#include "mongo/db/s/range_deletion_task_tracker.h"

namespace mongo {

namespace {
using Tasks = range_deletions::detail::Tasks;
const Tasks kEmptyTasks;

std::shared_ptr<RangeDeletion> getFirstOverlappingTask(const Tasks& tasks,
                                                       const ChunkRange& range) {
    for (const auto& [_, task] : tasks) {
        if (task->getRange().overlaps(range)) {
            return task;
        }
    }
    return nullptr;
}

void assertNoOverlappingRanges(const Tasks& tasks, const ChunkRange& range) {
    auto overlap = getFirstOverlappingTask(tasks, range);
    if (overlap == nullptr) {
        return;
    }
    uasserted(
        ErrorCodes::RangeOverlapConflict,
        fmt::format("Unable to register task for range {} because existing range {} is overlapping",
                    range.toBSON().toString(),
                    overlap->getRange().toBSON().toString()));
}

}  // namespace

using RegisteredTask = RangeDeletionTaskTracker::RegisteredTask;

RegisteredTask RangeDeletionTaskTracker::registerTask(const RangeDeletionTask& task) {
    auto& tasks = getTasksForCollectionMutable(task.getCollectionUuid());
    {
        auto it = tasks.find(task.getRange());
        if (it != tasks.end()) {
            return {it->second, RegistrationResult::kJoinedExistingTask};
        }
    }
    assertNoOverlappingRanges(tasks, task.getRange());
    auto [it, isNewTask] = tasks.emplace(task.getRange(), std::make_shared<RangeDeletion>(task));
    invariant(isNewTask);
    return {it->second, RegistrationResult::kRegisteredNewTask};
}

void RangeDeletionTaskTracker::completeTask(const UUID& collectionId, const ChunkRange& range) {
    auto collectionTasksIt = _collectionTasks.find(collectionId);
    if (collectionTasksIt == _collectionTasks.end()) {
        return;
    }
    auto& tasks = collectionTasksIt->second;
    auto tasksIt = tasks.find(range);
    if (tasksIt == tasks.end()) {
        return;
    }
    auto& task = tasksIt->second;
    task->markComplete();
    tasks.erase(tasksIt);
    if (tasks.empty()) {
        _collectionTasks.erase(collectionTasksIt);
    }
}

RangeDeletionTaskTracker::Tasks RangeDeletionTaskTracker::getOverlappingTasks(
    const UUID& collectionId, const ChunkRange& range) const {
    Tasks overlapping;
    for (const auto& [_, task] : getTasksForCollection(collectionId)) {
        if (task->getRange().overlaps(range)) {
            overlapping.emplace(task->getRange(), task);
        }
    }
    return overlapping;
}

size_t RangeDeletionTaskTracker::getTaskCount() const {
    size_t count = 0;
    for (const auto& [_, tasks] : _collectionTasks) {
        count += tasks.size();
    }
    return count;
}

size_t RangeDeletionTaskTracker::getTaskCountForCollection(const UUID& collectionId) const {
    return getTasksForCollection(collectionId).size();
}

void RangeDeletionTaskTracker::clear() {
    _collectionTasks.clear();
}

BSONObj RangeDeletionTaskTracker::getAllTasksBSON() const {
    BSONObjBuilder bob;
    for (const auto& [collId, tasks] : _collectionTasks) {
        BSONArrayBuilder section(bob.subarrayStart(collId.toString()));
        for (const auto& [range, _] : tasks) {
            section.append(range.toBSON());
        }
    }
    return bob.obj();
}

const Tasks& RangeDeletionTaskTracker::getTasksForCollection(const UUID& collectionId) const {
    auto it = _collectionTasks.find(collectionId);
    if (it == _collectionTasks.end()) {
        return kEmptyTasks;
    }
    return it->second;
}

Tasks& RangeDeletionTaskTracker::getTasksForCollectionMutable(const UUID& collectionId) {
    auto [it, _] = _collectionTasks.emplace(collectionId, Tasks{});
    return it->second;
}

}  // namespace mongo
