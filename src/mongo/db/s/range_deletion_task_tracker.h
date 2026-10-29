// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/s/range_deletion.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace range_deletions::detail {
/*
 * Hashes ChunkRange for use as keys in a hash map.
 *
 * NB: it ONLY makes sense to use this on ranges that are comparable, meaning
 * the ones based on the same key pattern (aka the ones belonging to the same
 * sharded collection).
 */
struct ChunkRangeHasher {
    size_t operator()(const ChunkRange& range) const {
        const auto& comparator = SimpleBSONObjComparator::kInstance;
        std::size_t seed = 0;
        comparator.hash_combine(seed, range.getMin());
        comparator.hash_combine(seed, range.getMax());
        return seed;
    }
};

using Tasks = stdx::unordered_map<ChunkRange, std::shared_ptr<RangeDeletion>, ChunkRangeHasher>;
using CollectionToTasks = stdx::unordered_map<UUID, Tasks, UUID::Hash>;
}  // namespace range_deletions::detail


class RangeDeletionTaskTracker {
public:
    using Tasks = range_deletions::detail::Tasks;

    enum RegistrationResult { kRegisteredNewTask, kJoinedExistingTask };

    struct RegisteredTask {
        std::shared_ptr<RangeDeletion> task;
        RegistrationResult result;
    };

    /**
     * Registers a new task, or joins an existing task if a task with the same ChunkRange already
     * exists. If a task is newly registered, the task pointer in the returned RegisteredTask will
     * represent the task parameter provided to this function, and result will be
     * kRegisteredNewTask. If a task with the same ChunkRange already exists, the task pointer in
     * the returned RegisteredTask will point to the previously registered task, and result will be
     * kJoinedExistingTask.
     */
    RangeDeletionTaskTracker::RegisteredTask registerTask(const RangeDeletionTask& task);

    /**
     * Removes a previously registered task with the given collectionId and ChunkRange from the set
     * of registered tasks. Returns the removed task, or nullptr if no task existed.
     */
    std::shared_ptr<RangeDeletion> removeTask(const UUID& collectionId, const ChunkRange& range);

    /**
     * Returns a map from ChunkRange to std::shared_ptr<RangeDeletion> containing all registered
     * tasks that have ChunkRanges overlapping the given range for the given collectionId.
     */
    Tasks getOverlappingTasks(const UUID& collectionId, const ChunkRange& range) const;

    /**
     * Returns the total number of registered tasks across all collections.
     */
    size_t getTaskCount() const;

    /**
     * Returns the total number of registered tasks for the given collectionId.
     */
    size_t getTaskCountForCollection(const UUID& collectionId) const;

    /**
     * Removes all registered tasks for all collections.
     */
    void clear();

    /**
     * Returns a BSONObj representing the ChunkRanges and collection UUIDs for all registered tasks.
     * Intended for informational use when logging or debugging.
     */
    BSONObj getAllTasksBSON() const;

private:
    const Tasks& getTasksForCollection(const UUID& collectionId) const;
    Tasks& getTasksForCollectionMutable(const UUID& collectionId);

    range_deletions::detail::CollectionToTasks _collectionTasks;
};

}  // namespace mongo
