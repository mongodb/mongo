/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

namespace mongo {
/**
 * This stage returns the index key or keys, used for executing the query, for each document in
 * the result. If the query does not use an index to perform the read operation, the stage returns
 * empty documents.
 *
 * If 'sortKeyMetaFields' vector is specified while constructing this stage, each element in this
 * array will be treated as a field name which will be added to the output document and will
 * hold a sort key for the document in the result set, if the sort key exists. The elements in this
 * array are the values specified in the 'sortKey' meta-projection.
 */
class ReturnKeyStage : public PlanStage {
public:
    static constexpr StringData kStageName = "RETURN_KEY"_sd;

    ReturnKeyStage(ExpressionContext* expCtx,
                   std::vector<FieldPath> sortKeyMetaFields,
                   WorkingSet* ws,
                   std::unique_ptr<PlanStage> child)
        : PlanStage(expCtx, std::move(child), kStageName.rawData()),
          _ws(*ws),
          _sortKeyMetaFields(std::move(sortKeyMetaFields)) {}

    StageType stageType() const final {
        return STAGE_RETURN_KEY;
    }

    bool isEOF() final {
        return child()->isEOF();
    }

    StageState doWork(WorkingSetID* out) final;

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

private:
    Status _extractIndexKey(WorkingSetMember* member);

    WorkingSet& _ws;
    ReturnKeyStats _specificStats;

    // The field names associated with any sortKey meta-projection(s). Empty if there is no sortKey
    // meta-projection.
    std::vector<FieldPath> _sortKeyMetaFields;
};
}  // namespace mongo
