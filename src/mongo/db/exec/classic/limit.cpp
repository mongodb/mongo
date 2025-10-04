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

#include "mongo/db/exec/classic/limit.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

using std::unique_ptr;

// static
const char* LimitStage::kStageType = "LIMIT";

LimitStage::LimitStage(ExpressionContext* expCtx,
                       long long limit,
                       WorkingSet*,
                       std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType, expCtx), _numToReturn(limit) {
    _specificStats.limit = _numToReturn;
    _children.emplace_back(std::move(child));
}

LimitStage::~LimitStage() {}

bool LimitStage::isEOF() const {
    return (0 == _numToReturn) || child()->isEOF();
}

PlanStage::StageState LimitStage::doWork(WorkingSetID* out) {
    if (0 == _numToReturn) {
        // We've returned as many results as we're limited to.
        return PlanStage::IS_EOF;
    }

    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        *out = id;
        --_numToReturn;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

unique_ptr<PlanStageStats> LimitStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_LIMIT);
    ret->specific = std::make_unique<LimitStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* LimitStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
