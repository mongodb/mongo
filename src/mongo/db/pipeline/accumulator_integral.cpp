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

#include "mongo/db/pipeline/accumulator_for_window_functions.h"

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"

namespace mongo {

REGISTER_WINDOW_FUNCTION(integral, mongo::window_function::ExpressionIntegral::parse);

AccumulatorIntegral::AccumulatorIntegral(ExpressionContext* const expCtx,
                                         boost::optional<long long> unitMillis)
    : AccumulatorForWindowFunctions(expCtx),
      _integralWF(expCtx, unitMillis, true /* isNonremovable */) {
    _memUsageBytes = sizeof(*this);
}

void AccumulatorIntegral::processInternal(const Value& input, bool merging) {
    tassert(5558800, "$integral can't be merged", !merging);

    _integralWF.add(input);
    _memUsageBytes = sizeof(*this) + _integralWF.getApproximateSize() - sizeof(_integralWF);
}

Value AccumulatorIntegral::getValue(bool toBeMerged) {
    return _integralWF.getValue();
}

void AccumulatorIntegral::reset() {
    _integralWF.reset();
    _memUsageBytes = sizeof(*this);
}

boost::intrusive_ptr<AccumulatorState> AccumulatorIntegral::create(
    ExpressionContext* const expCtx, boost::optional<long long> unitMillis) {
    return new AccumulatorIntegral(expCtx, unitMillis);
}

const char* AccumulatorIntegral::getOpName() const {
    return "$integral";
}

}  // namespace mongo
