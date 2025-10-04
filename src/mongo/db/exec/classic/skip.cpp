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

#include "mongo/db/exec/classic/skip.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

using std::unique_ptr;

// static
const char* SkipStage::kStageType = "SKIP";

SkipStage::SkipStage(ExpressionContext* expCtx,
                     long long toSkip,
                     WorkingSet* ws,
                     std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType, expCtx), _ws(ws), _leftToSkip(toSkip), _skipAmount(toSkip) {
    _children.emplace_back(std::move(child));
}

SkipStage::~SkipStage() {}

bool SkipStage::isEOF() const {
    return child()->isEOF();
}

PlanStage::StageState SkipStage::doWork(WorkingSetID* out) {
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        // If we're still skipping results...
        if (_leftToSkip > 0) {
            // ...drop the result.
            --_leftToSkip;
            _ws->free(id);
            return PlanStage::NEED_TIME;
        }

        *out = id;
        return PlanStage::ADVANCED;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    // NEED_TIME, NEED_YIELD, ERROR, IS_EOF
    return status;
}

unique_ptr<PlanStageStats> SkipStage::getStats() {
    _commonStats.isEOF = isEOF();
    _specificStats.skip = _skipAmount;
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_SKIP);
    ret->specific = std::make_unique<SkipStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SkipStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
