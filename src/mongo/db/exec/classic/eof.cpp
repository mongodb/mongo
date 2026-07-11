// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/eof.h"

#include <memory>

namespace mongo {

using std::unique_ptr;


EOFStage::EOFStage(ExpressionContext* expCtx, eof_node::EOFType type)
    : PlanStage(kStageType, expCtx), _specificStats(EofStats(type)) {}

EOFStage::~EOFStage() {}

bool EOFStage::isEOF() const {
    return true;
}

PlanStage::StageState EOFStage::doWork(WorkingSetID* out) {
    return PlanStage::IS_EOF;
}

unique_ptr<PlanStageStats> EOFStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_EOF);
    ret->specific = std::make_unique<EofStats>(_specificStats);
    return ret;
}

const SpecificStats* EOFStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
