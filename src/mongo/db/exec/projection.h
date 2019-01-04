
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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/projection_exec.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"

namespace mongo {

class CollatorInterface;

/**
 * This stage computes a projection. This is an abstract base class for various projection
 * implementations.
 */
class ProjectionStage : public PlanStage {
protected:
    ProjectionStage(OperationContext* opCtx,
                    const BSONObj& projObj,
                    WorkingSet* ws,
                    std::unique_ptr<PlanStage> child,
                    const char* stageType);

public:
    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

protected:
    using FieldSet = StringMap<bool>;  // Value is unused.

    /**
     * Given the projection spec for a simple inclusion projection,
     * 'projObj', populates 'includedFields' with the set of field
     * names to be included.
     */
    static void getSimpleInclusionFields(const BSONObj& projObj, FieldSet* includedFields);

    bool projObjHasOwnedData() {
        return _projObj.isOwned() && !_projObj.isEmpty();
    }

    // The projection object used by all projection implementations. We lack a ProjectionExpression
    // or similar so we use a BSONObj.
    BSONObj _projObj;

private:
    /**
     * Runs either the default complete implementation or a fast path depending on how this was
     * constructed.
     */
    virtual Status transform(WorkingSetMember* member) const = 0;

    // Used to retrieve a WorkingSetMember as part of 'doWork()'.
    WorkingSet& _ws;

    // Populated by 'getStats()'.
    ProjectionStats _specificStats;
};

/**
 * The default case. Can handle every projection.
 */
class ProjectionStageDefault final : public ProjectionStage {
public:
    /**
     * ProjectionNodeDefault should use this for construction.
     */
    ProjectionStageDefault(OperationContext* opCtx,
                           const BSONObj& projObj,
                           WorkingSet* ws,
                           std::unique_ptr<PlanStage> child,
                           const MatchExpression& fullExpression,
                           const CollatorInterface* collator);

    StageType stageType() const final {
        return STAGE_PROJECTION_DEFAULT;
    }

private:
    Status transform(WorkingSetMember* member) const final;

    // Fully-general heavy execution object.
    ProjectionExec _exec;
};

/**
 * This class is used when the projection is totally covered by one index and the following rules
 * are met: the projection consists only of inclusions e.g. '{field: 1}', it has no $meta
 * projections, it is not a returnKey projection and it has no dotted fields.
 */
class ProjectionStageCovered final : public ProjectionStage {
public:
    /**
     * ProjectionNodeCovered should obtain a fast-path object through this constructor.
     */
    ProjectionStageCovered(OperationContext* opCtx,
                           const BSONObj& projObj,
                           WorkingSet* ws,
                           std::unique_ptr<PlanStage> child,
                           const BSONObj& coveredKeyObj);

    StageType stageType() const final {
        return STAGE_PROJECTION_COVERED;
    }

private:
    Status transform(WorkingSetMember* member) const final;

    // Has the field names present in the simple projection.
    FieldSet _includedFields;

    // This is the key pattern we're extracting covered data from. It is maintained here since
    // strings derived from it depend on its lifetime.
    BSONObj _coveredKeyObj;

    // Field names can be empty in 2.4 and before so we can't use them as a sentinel value.
    // If the i-th entry is true we include the i-th field in the key.
    std::vector<bool> _includeKey;

    // If the i-th entry of _includeKey is true this is the field name for the i-th key field.
    std::vector<StringData> _keyFieldNames;
};

/**
 * This class is used when we expect an object and the following rules are met: the projection
 * consists only of inclusions e.g. '{field: 1}', it has no $meta projections, it is not a returnKey
 * projection and it has no dotted fields.
 */
class ProjectionStageSimple final : public ProjectionStage {
public:
    /**
     * ProjectionNodeSimple should obtain a fast-path object through this constructor.
     */
    ProjectionStageSimple(OperationContext* opCtx,
                          const BSONObj& projObj,
                          WorkingSet* ws,
                          std::unique_ptr<PlanStage> child);

    StageType stageType() const final {
        return STAGE_PROJECTION_SIMPLE;
    }

private:
    Status transform(WorkingSetMember* member) const final;

    // Has the field names present in the simple projection.
    FieldSet _includedFields;
};

}  // namespace mongo
