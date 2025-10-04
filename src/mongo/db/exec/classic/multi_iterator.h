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

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/record_store.h"

#include <memory>
#include <vector>

namespace mongo {

/**
 * Iterates over a collection using multiple underlying RecordCursors.
 *
 * This is a special stage which is not used automatically by queries. It is intended for special
 * commands that work with RecordCursors.
 */
class MultiIteratorStage final : public RequiresCollectionStage {
public:
    MultiIteratorStage(ExpressionContext* expCtx, WorkingSet* ws, CollectionAcquisition collection);

    void addIterator(std::unique_ptr<RecordCursor> it);

    PlanStage::StageState doWork(WorkingSetID* out) final;

    bool isEOF() const final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    // Returns empty PlanStageStats object
    std::unique_ptr<PlanStageStats> getStats() final;

    // Not used.
    SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    // Not used.
    StageType stageType() const final {
        return STAGE_MULTI_ITERATOR;
    }

    static const char* kStageType;

protected:
    void doSaveStateRequiresCollection() final;

    void doRestoreStateRequiresCollection() final;

private:
    std::vector<std::unique_ptr<RecordCursor>> _iterators;

    // Not owned by us.
    WorkingSet* _ws;
};

}  // namespace mongo
