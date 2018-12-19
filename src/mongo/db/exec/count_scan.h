
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
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

class WorkingSet;

struct CountScanParams {
    CountScanParams(const IndexDescriptor* descriptor,
                    std::string indexName,
                    BSONObj keyPattern,
                    MultikeyPaths multikeyPaths,
                    bool multikey)
        : indexDescriptor(descriptor),
          name(std::move(indexName)),
          keyPattern(std::move(keyPattern)),
          multikeyPaths(std::move(multikeyPaths)),
          isMultiKey(multikey) {
        invariant(descriptor);
    }

    CountScanParams(OperationContext* opCtx, const IndexDescriptor* descriptor)
        : CountScanParams(descriptor,
                          descriptor->indexName(),
                          descriptor->keyPattern(),
                          descriptor->getMultikeyPaths(opCtx),
                          descriptor->isMultikey(opCtx)) {}

    const IndexDescriptor* indexDescriptor;
    std::string name;

    BSONObj keyPattern;

    MultikeyPaths multikeyPaths;
    bool isMultiKey;

    BSONObj startKey;
    bool startKeyInclusive{true};

    BSONObj endKey;
    bool endKeyInclusive{true};
};

/**
 * Used by the count command. Scans an index from a start key to an end key. Creates a
 * WorkingSetMember for each matching index key in RID_AND_OBJ state. It has a null record id and an
 * empty object with a null snapshot id rather than real data. Returning real data is unnecessary
 * since all we need is the count.
 *
 * Only created through the getExecutorCount() path, as count is the only operation that doesn't
 * care about its data.
 */
class CountScan final : public RequiresIndexStage {
public:
    CountScan(OperationContext* opCtx, CountScanParams params, WorkingSet* workingSet);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_COUNT_SCAN;
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

    const bool _shouldDedup;

    const BSONObj _startKey;
    const bool _startKeyInclusive = true;

    const BSONObj _endKey;
    const bool _endKeyInclusive = true;

    std::unique_ptr<SortedDataInterface::Cursor> _cursor;

    // The set of record ids we've returned so far. Used to avoid returning duplicates, if
    // '_shouldDedup' is set to true.
    stdx::unordered_set<RecordId, RecordId::Hasher> _returned;

    CountScanStats _specificStats;
};

}  // namespace mongo
