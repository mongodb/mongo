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

#include "mongo/db/exec/classic/eof.h"

#include <memory>

namespace mongo {

using std::unique_ptr;

// static
const char* EOFStage::kStageType = "EOF";

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
