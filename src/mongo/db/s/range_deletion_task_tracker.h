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

#pragma once

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/s/range_deletion.h"
#include "mongo/stdx/unordered_map.h"

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
     * kJoinedExistingTask. An exception will be thrown if a new task is attempted to be registered
     * that overlaps, but does not exactly match, an existing task.
     */
    RangeDeletionTaskTracker::RegisteredTask registerTask(const RangeDeletionTask& task);

    /**
     * Removes a previously registered task with the given collectionId and ChunkRange from the set
     * of registered tasks and calls markComplete() on it. No-op if no matching task exists.
     */
    void completeTask(const UUID& collectionId, const ChunkRange& range);

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
