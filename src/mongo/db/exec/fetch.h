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

#include <memory>

#include "mongo/db/exec/requires_collection_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"

namespace mongo {

class SeekableRecordCursor;

/**
 * This stage turns a RecordId into a BSONObj.
 *
 * In WorkingSetMember terms, it transitions from RID_AND_IDX to RID_AND_OBJ by reading
 * the record at the provided RecordId.  Returns verbatim any data that already has an object.
 *
 * Preconditions: Valid RecordId.
 */
class FetchStage : public RequiresCollectionStage {
public:
    FetchStage(OperationContext* opCtx,
               WorkingSet* ws,
               std::unique_ptr<PlanStage> child,
               const MatchExpression* filter,
               const Collection* collection);

    ~FetchStage();

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_FETCH;
    }

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

protected:
    void doSaveStateRequiresCollection() final;

    void doRestoreStateRequiresCollection() final;

private:
    /**
     * If the member (with id memberID) passes our filter, set *out to memberID and return that
     * ADVANCED.  Otherwise, free memberID and return NEED_TIME.
     */
    StageState returnIfMatches(WorkingSetMember* member, WorkingSetID memberID, WorkingSetID* out);

    // Used to fetch Records from _collection.
    std::unique_ptr<SeekableRecordCursor> _cursor;

    // _ws is not owned by us.
    WorkingSet* _ws;

    // The filter is not owned by us.
    const MatchExpression* _filter;

    // If not Null, we use this rather than asking our child what to do next.
    WorkingSetID _idRetrying;

    // Stats
    FetchStats _specificStats;
};

}  // namespace mongo
