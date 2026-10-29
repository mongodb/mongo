// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/range_deletion_task_tracker.h"

namespace mongo {

namespace {
using Tasks = range_deletions::detail::Tasks;
const Tasks kEmptyTasks;
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
    auto [it, isNewTask] = tasks.emplace(task.getRange(), std::make_shared<RangeDeletion>(task));
    invariant(isNewTask);
    return {it->second, RegistrationResult::kRegisteredNewTask};
}

std::shared_ptr<RangeDeletion> RangeDeletionTaskTracker::removeTask(const UUID& collectionId,
                                                                    const ChunkRange& range) {
    auto collectionTasksIt = _collectionTasks.find(collectionId);
    if (collectionTasksIt == _collectionTasks.end()) {
        return nullptr;
    }
    auto& tasks = collectionTasksIt->second;
    auto tasksIt = tasks.find(range);
    if (tasksIt == tasks.end()) {
        return nullptr;
    }
    auto task = tasksIt->second;
    tasks.erase(tasksIt);
    if (tasks.empty()) {
        _collectionTasks.erase(collectionTasksIt);
    }
    return task;
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
