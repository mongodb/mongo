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

#include <absl/container/inlined_vector.h>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/util/assert_util_core.h"

namespace mongo::sbe {
LimitSkipStage::LimitSkipStage(std::unique_ptr<PlanStage> input,
                               std::unique_ptr<EExpression> limit,
                               std::unique_ptr<EExpression> skip,
                               PlanNodeId planNodeId,
                               bool participateInTrialRunTracking)
    : PlanStage(!skip ? "limit"_sd : "limitskip"_sd,
                nullptr /* yieldPolicy */,
                planNodeId,
                participateInTrialRunTracking),
      _limitExpr(std::move(limit)),
      _skipExpr(std::move(skip)),
      _current(0),
      _isEOF(false) {
    invariant(_limitExpr || _skipExpr);
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> LimitSkipStage::clone() const {
    return std::make_unique<LimitSkipStage>(_children[0]->clone(),
                                            _limitExpr ? _limitExpr->clone() : nullptr,
                                            _skipExpr ? _skipExpr->clone() : nullptr,
                                            _commonStats.nodeId,
                                            _participateInTrialRunTracking);
}

void LimitSkipStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    if (_limitExpr) {
        _limitCode = _limitExpr->compile(ctx);
    }
    if (_skipExpr) {
        _skipCode = _skipExpr->compile(ctx);
    }
}

value::SlotAccessor* LimitSkipStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _children[0]->getAccessor(ctx, slot);
}

void LimitSkipStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _isEOF = false;
    _children[0]->open(reOpen);

    _limit = _runLimitOrSkipCode(_limitCode.get());
    _skip = _runLimitOrSkipCode(_skipCode.get());
    _specificStats.limit = _limit;
    _specificStats.skip = _skip;

    if (_skip) {
        for (_current = 0; _current < *_skip && !_isEOF; _current++) {
            _isEOF = _children[0]->getNext() == PlanState::IS_EOF;
        }
    }
    _current = 0;
}
PlanState LimitSkipStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_isEOF || (_limit && _current++ == *_limit)) {
        return trackPlanState(PlanState::IS_EOF);
    }

    return trackPlanState(_children[0]->getNext());
}
void LimitSkipStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> LimitSkipStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<LimitSkipStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        if (_limit) {
            bob.appendNumber("limit", static_cast<long long>(*_limit));
        }
        if (_skip) {
            bob.appendNumber("skip", static_cast<long long>(*_skip));
        }
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* LimitSkipStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> LimitSkipStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();
    if (!_skipExpr) {
        DebugPrinter::addBlocks(ret, _limitExpr->debugPrint());
    } else {
        if (_limitExpr) {
            DebugPrinter::addBlocks(ret, _limitExpr->debugPrint());
        } else {
            ret.emplace_back("none");
        }
        DebugPrinter::addBlocks(ret, _skipExpr->debugPrint());
    }
    DebugPrinter::addNewLine(ret);

    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t LimitSkipStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_specificStats);
    size += _limitExpr ? _limitExpr->estimateSize() : 0;
    size += _skipExpr ? _skipExpr->estimateSize() : 0;
    return size;
}

boost::optional<int64_t> LimitSkipStage::_runLimitOrSkipCode(const vm::CodeFragment* code) {
    if (code == nullptr) {
        return boost::none;
    }

    auto [owned, tag, val] = _bytecode.run(code);
    value::ValueGuard guard{owned, tag, val};
    tassert(8349200,
            "Expect limit or skip code to return an int64",
            tag == value::TypeTags::NumberInt64);
    return value::bitcastTo<int64_t>(val);
}

}  // namespace mongo::sbe
