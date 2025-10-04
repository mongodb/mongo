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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sort_executor.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"

#include <cstdint>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Sorts the input received from the child according to the sort pattern provided. If
 * 'addSortKeyMetadata' is true, then also attaches the sort key as metadata. This could be consumed
 * downstream for a sort-merge on a merging node, or by a $meta:"sortKey" expression.
 *
 * Concrete implementations derive from this abstract base class by implementing methods for
 * spooling and unspooling.
 */
class SortStage : public PlanStage {
public:
    static constexpr StringData kStageType = "SORT"_sd;

    SortStage(boost::intrusive_ptr<ExpressionContext> expCtx,
              WorkingSet* ws,
              SortPattern sortPattern,
              bool addSortKeyMetadata,
              std::unique_ptr<PlanStage> child);

    /**
     * Loads the WorkingSetMember pointed to by 'wsid' into the set of objects being sorted. This
     * should be called repeatedly until all documents are loaded, followed by a single call to
     * 'loadingDone()'. Illegal to call after 'loadingDone()' has been called.
     */
    virtual void spool(WorkingSetID wsid) = 0;

    /**
     * Indicates that all documents to be sorted have been loaded via 'spool()'. This method must
     * not be called more than once.
     */
    virtual void loadingDone() = 0;

    /**
     * Returns an id referring to the next WorkingSetMember in the sorted stream of results.
     *
     * If there is another WSM, the id is returned via the out-parameter and the return value is
     * PlanStage::ADVANCED. If there are no more documents remaining in the sorted stream, returns
     * PlanStage::IS_EOF, and 'out' is left unmodified.
     *
     * Illegal to call before 'loadingDone()' has been called.
     */
    virtual StageState unspool(WorkingSetID* out) = 0;

    StageState doWork(WorkingSetID* out) final;

    std::unique_ptr<PlanStageStats> getStats() final;

    const SimpleMemoryUsageTracker& getMemoryUsageTracker_forTest() const {
        return _memoryTracker;
    }

protected:
    // Not owned by us.
    WorkingSet* _ws;

    const SortKeyGenerator _sortKeyGen;

    const bool _addSortKeyMetadata;

    SimpleMemoryUsageTracker _memoryTracker;

private:
    // Whether or not we have finished loading data into '_sortExecutor'.
    bool _populated = false;
};

/**
 * Generic sorting implementation which can handle sorting any WorkingSetMember, including those
 * that have RecordIds, metadata, or which represent index keys.
 */
class SortStageDefault final : public SortStage {
public:
    SortStageDefault(boost::intrusive_ptr<ExpressionContext> expCtx,
                     WorkingSet* ws,
                     SortPattern sortPattern,
                     uint64_t limit,
                     uint64_t maxMemoryUsageBytes,
                     bool addSortKeyMetadata,
                     std::unique_ptr<PlanStage> child);

    void spool(WorkingSetID wsid) final;

    void loadingDone() final;

    StageState unspool(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SORT_DEFAULT;
    }

    bool isEOF() const final {
        return _sortExecutor.isEOF();
    }

    const SpecificStats* getSpecificStats() const final {
        return &_sortExecutor.stats();
    }

    void doForceSpill() final {
        _sortExecutor.forceSpill();
    }

private:
    SortExecutor<SortableWorkingSetMember> _sortExecutor;
};

/**
 * Optimized sorting implementation which can be used for WorkingSetMembers in a fetched state that
 * have no metadata. This implementation is faster but less general than WorkingSetMemberSortStage.
 *
 * For performance, this implementation discards record ids and returns WorkingSetMembers in the
 * OWNED_OBJ state. Therefore, this sort implementation cannot be used if the plan requires the
 * record id to be preserved (e.g. for update or delete plans, where an ancestor stage needs to
 * refer to the record in order to perform a write).
 */
class SortStageSimple final : public SortStage {
public:
    SortStageSimple(boost::intrusive_ptr<ExpressionContext> expCtx,
                    WorkingSet* ws,
                    SortPattern sortPattern,
                    uint64_t limit,
                    uint64_t maxMemoryUsageBytes,
                    bool addSortKeyMetadata,
                    std::unique_ptr<PlanStage> child);

    void spool(WorkingSetID wsid) final;

    void loadingDone() final;

    StageState unspool(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_SORT_SIMPLE;
    }

    bool isEOF() const final {
        return _sortExecutor.isEOF();
    }

    const SpecificStats* getSpecificStats() const final {
        return &_sortExecutor.stats();
    }

    void doForceSpill() final {
        _sortExecutor.forceSpill();
    }

private:
    SortExecutor<BSONObj> _sortExecutor;
};

}  // namespace mongo
