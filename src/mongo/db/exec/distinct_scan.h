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


#include "mongo/db/exec/requires_index_stage.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/record_id.h"

namespace mongo {

class IndexAccessMethod;
class IndexDescriptor;
class WorkingSet;

struct DistinctParams {
    DistinctParams(const IndexDescriptor* descriptor,
                   std::string indexName,
                   BSONObj keyPattern,
                   MultikeyPaths multikeyPaths,
                   bool multikey)
        : indexDescriptor(descriptor),
          name(std::move(indexName)),
          keyPattern(std::move(keyPattern)),
          multikeyPaths(std::move(multikeyPaths)),
          isMultiKey(multikey) {
        invariant(indexDescriptor);
    }

    DistinctParams(OperationContext* opCtx, const IndexDescriptor* descriptor)
        : DistinctParams(descriptor,
                         descriptor->indexName(),
                         descriptor->keyPattern(),
                         descriptor->getMultikeyPaths(opCtx),
                         descriptor->isMultikey()) {}

    const IndexDescriptor* indexDescriptor;
    std::string name;

    BSONObj keyPattern;

    MultikeyPaths multikeyPaths;
    bool isMultiKey;

    int scanDirection{1};

    // What are the bounds?
    IndexBounds bounds;

    // What field in the index's key pattern is the one we're distinct-ing over?
    // For example:
    // If we have an index {a:1, b:1} we could use it to distinct over either 'a' or 'b'.
    // If we distinct over 'a' the position is 0.
    // If we distinct over 'b' the position is 1.
    int fieldNo{0};
};

/**
 * Used by the distinct command.  Executes a mutated index scan over the provided bounds.
 * However, rather than looking at every key in the bounds, it skips to the next value of the
 * _params.fieldNo-th indexed field.  This is because distinct only cares about distinct values
 * for that field, so there is no point in examining all keys with the same value for that
 * field.
 *
 * Only created through the getExecutorDistinct path.  See db/query/get_executor.cpp
 */
class DistinctScan final : public RequiresIndexStage {
public:
    DistinctScan(OperationContext* opCtx, DistinctParams params, WorkingSet* workingSet);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_DISTINCT_SCAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

protected:
    void doSaveStateRequiresIndex() final;

    void doRestoreStateRequiresIndex() final;

private:
    // The WorkingSet we annotate with results.  Not owned by us.
    WorkingSet* _workingSet;

    const BSONObj _keyPattern;

    const int _scanDirection = 1;

    const IndexBounds _bounds;

    const int _fieldNo = 0;

    // The cursor we use to navigate the tree.
    std::unique_ptr<SortedDataInterface::Cursor> _cursor;

    // _checker gives us our start key and ensures we stay in bounds.
    IndexBoundsChecker _checker;
    IndexSeekPoint _seekPoint;

    // Stats
    DistinctScanStats _specificStats;
};

}  // namespace mongo
