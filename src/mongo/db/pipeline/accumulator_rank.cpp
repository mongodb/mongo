/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <cmath>
#include <limits>

#include "mongo/db/pipeline/accumulator_for_window_functions.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/util/summation.h"

namespace mongo {

using boost::intrusive_ptr;

// These don't make sense as accumulators, so only register them as window functions.
REGISTER_WINDOW_FUNCTION(
    rank, mongo::window_function::ExpressionFromRankAccumulator<AccumulatorRank>::parse);
REGISTER_WINDOW_FUNCTION(
    denseRank, mongo::window_function::ExpressionFromRankAccumulator<AccumulatorDenseRank>::parse);
REGISTER_WINDOW_FUNCTION(
    documentNumber,
    mongo::window_function::ExpressionFromRankAccumulator<AccumulatorDocumentNumber>::parse);

const char* AccumulatorRank::getOpName() const {
    return "$rank";
}

void AccumulatorRank::processInternal(const Value& input, bool merging) {
    tassert(5417001, "$rank can't be merged", !merging);
    if (!_lastInput ||
        getExpressionContext()->getValueComparator().compare(_lastInput.get(), input) != 0) {
        _lastRank += _numSameRank;
        _numSameRank = 1;
        _lastInput = input;
        _memUsageBytes = sizeof(*this) + _lastInput->getApproximateSize() - sizeof(Value);
    } else {
        ++_numSameRank;
    }
}

const char* AccumulatorDocumentNumber::getOpName() const {
    return "$documentNumber";
}

void AccumulatorDocumentNumber::processInternal(const Value& input, bool merging) {
    tassert(5417002, "$documentNumber can't be merged", !merging);
    // DocumentNumber doesn't need to keep track of what we just saw.
    ++_lastRank;
}

const char* AccumulatorDenseRank::getOpName() const {
    return "$denseRank";
}

void AccumulatorDenseRank::processInternal(const Value& input, bool merging) {
    tassert(5417003, "$denseRank can't be merged", !merging);
    if (!_lastInput ||
        getExpressionContext()->getValueComparator().compare(_lastInput.get(), input) != 0) {
        ++_lastRank;
        _lastInput = input;
        _memUsageBytes = sizeof(*this) + _lastInput->getApproximateSize() - sizeof(Value);
    }
}

intrusive_ptr<AccumulatorState> AccumulatorRank::create(ExpressionContext* const expCtx) {
    return new AccumulatorRank(expCtx);
}

intrusive_ptr<AccumulatorState> AccumulatorDenseRank::create(ExpressionContext* const expCtx) {
    return new AccumulatorDenseRank(expCtx);
}

intrusive_ptr<AccumulatorState> AccumulatorDocumentNumber::create(ExpressionContext* const expCtx) {
    return new AccumulatorDocumentNumber(expCtx);
}

AccumulatorRankBase::AccumulatorRankBase(ExpressionContext* const expCtx)
    : AccumulatorForWindowFunctions(expCtx) {
    _memUsageBytes = sizeof(*this);
}

void AccumulatorRankBase::reset() {
    _lastInput = boost::none;
    _lastRank = 0;
}

void AccumulatorRank::reset() {
    _lastInput = boost::none;
    _numSameRank = 1;
    _lastRank = 0;
}
}  // namespace mongo
