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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/limit_skip.h"

namespace mongo::sbe {
LimitSkipStage::LimitSkipStage(std::unique_ptr<PlanStage> input,
                               boost::optional<long long> limit,
                               boost::optional<long long> skip,
                               PlanNodeId planNodeId)
    : PlanStage(!skip ? "limit"_sd : "limitskip"_sd, planNodeId),
      _limit(limit),
      _skip(skip),
      _current(0),
      _isEOF(false) {
    invariant(_limit || _skip);
    _children.emplace_back(std::move(input));
    _specificStats.limit = limit;
    _specificStats.skip = skip;
}

std::unique_ptr<PlanStage> LimitSkipStage::clone() const {
    return std::make_unique<LimitSkipStage>(
        _children[0]->clone(), _limit, _skip, _commonStats.nodeId);
}

void LimitSkipStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
}

value::SlotAccessor* LimitSkipStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _children[0]->getAccessor(ctx, slot);
}

void LimitSkipStage::open(bool reOpen) {
    _commonStats.opens++;
    _isEOF = false;
    _children[0]->open(reOpen);

    if (_skip) {
        for (_current = 0; _current < *_skip && !_isEOF; _current++) {
            _isEOF = _children[0]->getNext() == PlanState::IS_EOF;
        }
    }
    _current = 0;
}
PlanState LimitSkipStage::getNext() {
    if (_isEOF || (_limit && _current++ == *_limit)) {
        return trackPlanState(PlanState::IS_EOF);
    }

    return trackPlanState(_children[0]->getNext());
}
void LimitSkipStage::close() {
    _commonStats.closes++;
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> LimitSkipStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<LimitSkipStats>(_specificStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* LimitSkipStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> LimitSkipStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();
    if (!_skip) {
        ret.emplace_back(std::to_string(*_limit));
    } else {
        ret.emplace_back(_limit ? std::to_string(*_limit) : "none");
        ret.emplace_back(std::to_string(*_skip));
    }
    DebugPrinter::addNewLine(ret);

    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}
}  // namespace mongo::sbe
