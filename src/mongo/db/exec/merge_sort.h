/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <list>
#include <queue>
#include <vector>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"

namespace mongo {

class CollatorInterface;
// External params for the merge sort stage.  Declared below.
class MergeSortStageParams;

/**
 * Merges the outputs of N children, each of which is sorted in the order specified by
 * 'pattern'.  The output is sorted by 'pattern'.  Practically speaking, all of this stage's
 * children are indices.
 *
 * AKA the SERVER-1205 stage.  Allows very efficient handling of the following query:
 * find($or[{a:1}, {b:1}]).sort({c:1}) with indices {a:1, c:1} and {b:1, c:1}.
 *
 * Preconditions: For each field in 'pattern' all inputs in the child must handle a
 * getFieldDotted for that field.
 */
class MergeSortStage final : public PlanStage {
public:
    MergeSortStage(OperationContext* opCtx, const MergeSortStageParams& params, WorkingSet* ws);

    void addChild(PlanStage* child);

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SORT_MERGE;
    }

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    struct StageWithValue {
        StageWithValue() : id(WorkingSet::INVALID_ID), stage(nullptr) {}
        WorkingSetID id;
        PlanStage* stage;
    };

    // This stage maintains a priority queue of results from each child stage so that it can quickly
    // return the next result according to the sort order. A value in the priority queue is a
    // MergingRef, an iterator which refers to a buffered (WorkingSetMember, child stage) pair.
    typedef std::list<StageWithValue>::iterator MergingRef;

    // The comparison function used in our priority queue.
    class StageWithValueComparison {
    public:
        StageWithValueComparison(WorkingSet* ws, BSONObj pattern, const CollatorInterface* collator)
            : _ws(ws), _pattern(pattern), _collator(collator) {}

        // Is lhs less than rhs?  Note that priority_queue is a max heap by default so we invert
        // the return from the expected value.
        bool operator()(const MergingRef& lhs, const MergingRef& rhs);

    private:
        WorkingSet* _ws;
        BSONObj _pattern;
        const CollatorInterface* _collator;
    };

    // Not owned by us.
    WorkingSet* _ws;

    // The pattern that we're sorting by.
    BSONObj _pattern;

    // Null if this merge sort stage orders strings according to simple binary compare. If non-null,
    // represents the collator used to compare strings.
    const CollatorInterface* _collator;

    // Are we deduplicating on RecordId?
    const bool _dedup;

    // Which RecordIds have we seen?
    stdx::unordered_set<RecordId, RecordId::Hasher> _seen;

    // In order to pick the next smallest value, we need each child work(...) until it produces
    // a result.  This is the queue of children that haven't given us a result yet.
    std::queue<PlanStage*> _noResultToMerge;

    // The min heap of the results we're returning.
    std::priority_queue<MergingRef, std::vector<MergingRef>, StageWithValueComparison> _merging;

    // The data referred to by the _merging queue above.
    std::list<StageWithValue> _mergingData;

    // Stats
    MergeSortStats _specificStats;
};

// Parameters that must be provided to a MergeSortStage
class MergeSortStageParams {
public:
    MergeSortStageParams() : collator(nullptr), dedup(true) {}

    // How we're sorting.
    BSONObj pattern;

    // Null if this merge sort stage orders strings according to simple binary compare. If non-null,
    // represents the collator used to compare strings.
    const CollatorInterface* collator;

    // Do we deduplicate on RecordId?
    bool dedup;
};

}  // namespace mongo
