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

#include "mongo/unittest/unittest.h"

#define ASSERT_UASSERTS(STATEMENT) ASSERT_THROWS(STATEMENT, AssertionException)

namespace mongo {

RangeDeletionTask createTask(UUID collectionId, ChunkRange range) {
    RangeDeletionTask task;
    task.setCollectionUuid(std::move(collectionId));
    task.setRange(std::move(range));
    return task;
}

const ChunkRange kTestRange{BSON("x" << 10), BSON("x" << 20)};
const ChunkRange kDisjointRange{BSON("x" << 1), BSON("x" << 2)};
const ChunkRange kLeftExactRange{BSON("x" << 10), BSON("x" << 15)};
const ChunkRange kLeftMoreRange{BSON("x" << 5), BSON("x" << 15)};
const ChunkRange kRightExactRange{BSON("x" << 15), BSON("x" << 20)};
const ChunkRange kRightMoreRange{BSON("x" << 15), BSON("x" << 25)};
const ChunkRange kCoverRange{BSON("x" << 5), BSON("x" << 25)};
const ChunkRange kBisectRange{BSON("x" << 12), BSON("x" << 17)};

std::vector<ChunkRange> getOverlappingRangeCases() {
    std::vector<ChunkRange> ranges;
    ranges.emplace_back(kTestRange);
    ranges.emplace_back(kLeftExactRange);
    ranges.emplace_back(kLeftMoreRange);
    ranges.emplace_back(kRightExactRange);
    ranges.emplace_back(kRightMoreRange);
    ranges.emplace_back(kCoverRange);
    ranges.emplace_back(kBisectRange);

    std::vector<ChunkRange> refinedRanges;
    for (const auto& range : ranges) {
        auto min = range.getMin().addField(BSON("y" << 50).firstElement());
        auto max = range.getMax().addField(BSON("y" << 100).firstElement());
        refinedRanges.emplace_back(min, max);
    }
    ranges.insert(ranges.end(), refinedRanges.begin(), refinedRanges.end());
    return ranges;
}

TEST(RangeDeletionTaskTracker, RegisterNewTask) {
    RangeDeletionTaskTracker tasks;
    auto task = createTask(UUID::gen(), kTestRange);
    auto registration = tasks.registerTask(task);
    ASSERT_EQ(registration.result, RangeDeletionTaskTracker::kRegisteredNewTask);
}

TEST(RangeDeletionTaskTracker, JoinExistingTask) {
    RangeDeletionTaskTracker tasks;
    auto task = createTask(UUID::gen(), kTestRange);
    auto firstRegistration = tasks.registerTask(task);
    ASSERT_EQ(firstRegistration.result, RangeDeletionTaskTracker::kRegisteredNewTask);
    auto secondRegistration = tasks.registerTask(task);
    ASSERT_EQ(secondRegistration.result, RangeDeletionTaskTracker::kJoinedExistingTask);
    ASSERT_EQ(firstRegistration.task, secondRegistration.task);
}

TEST(RangeDeletionTaskTracker, SameRangeDifferentCollection) {
    RangeDeletionTaskTracker tasks;
    auto task = createTask(UUID::gen(), kTestRange);
    auto firstRegistration = tasks.registerTask(task);
    ASSERT_EQ(firstRegistration.result, RangeDeletionTaskTracker::kRegisteredNewTask);
    task.setCollectionUuid(UUID::gen());
    auto secondRegistration = tasks.registerTask(task);
    ASSERT_EQ(secondRegistration.result, RangeDeletionTaskTracker::kRegisteredNewTask);
    ASSERT_NE(firstRegistration.task, secondRegistration.task);
}

TEST(RangeDeletionTaskTracker, RegisterOverlappingTaskUasserts) {
    auto collId = UUID::gen();
    auto originalTask = createTask(collId, {BSON("x" << 10), BSON("x" << 20)});
    RangeDeletionTaskTracker tasks;
    tasks.registerTask(originalTask);

    for (const auto& range : getOverlappingRangeCases()) {
        if (range == originalTask.getRange()) {
            // Exact overlap joins the existing task.
            continue;
        }
        ASSERT_UASSERTS(tasks.registerTask(createTask(collId, range))) << range.toBSON();
    }
}

TEST(RangeDeletionTaskTracker, GetOverlappingTasks) {
    auto collId = UUID::gen();
    for (const auto& range : getOverlappingRangeCases()) {
        RangeDeletionTaskTracker tasks;
        tasks.registerTask(createTask(collId, range));
        auto overlap = tasks.getOverlappingTasks(collId, kTestRange);
        ASSERT_EQ(overlap.size(), 1) << range.toBSON();
    }
}

TEST(RangeDeletionTaskTracker, MultipleOverlappingTasks) {
    auto collId = UUID::gen();
    RangeDeletionTaskTracker tasks;
    tasks.registerTask(createTask(collId, {BSON("x" << 10), BSON("x" << 15)}));
    tasks.registerTask(createTask(collId, {BSON("x" << 15), BSON("x" << 20)}));
    auto overlap = tasks.getOverlappingTasks(collId, {BSON("x" << 10), BSON("x" << 20)});
    ASSERT_EQ(overlap.size(), 2);
}

TEST(RangeDeletionTaskTracker, TaskCounts) {
    RangeDeletionTaskTracker tasks;
    ASSERT_EQ(tasks.getTaskCount(), 0);
    ASSERT_EQ(tasks.getTaskCountForCollection(UUID::gen()), 0);

    auto task = createTask(UUID::gen(), kTestRange);
    tasks.registerTask(task);
    ASSERT_EQ(tasks.getTaskCount(), 1);
    ASSERT_EQ(tasks.getTaskCountForCollection(task.getCollectionUuid()), 1);

    task.setRange(kDisjointRange);
    tasks.registerTask(task);
    ASSERT_EQ(tasks.getTaskCount(), 2);
    ASSERT_EQ(tasks.getTaskCountForCollection(task.getCollectionUuid()), 2);

    task.setCollectionUuid(UUID::gen());
    tasks.registerTask(task);
    ASSERT_EQ(tasks.getTaskCount(), 3);
    ASSERT_EQ(tasks.getTaskCountForCollection(task.getCollectionUuid()), 1);

    tasks.clear();
    ASSERT_EQ(tasks.getTaskCount(), 0);
    ASSERT_EQ(tasks.getTaskCountForCollection(task.getCollectionUuid()), 0);
}

TEST(RangeDeletionTaskTracker, CompleteTask) {
    RangeDeletionTaskTracker tasks;
    auto task = createTask(UUID::gen(), kTestRange);
    auto [registeredTask, _] = tasks.registerTask(task);
    tasks.completeTask(task.getCollectionUuid(), task.getRange());
    ASSERT_EQ(tasks.getTaskCount(), 0);
    registeredTask->getCompletionFuture().get();
}

}  // namespace mongo
