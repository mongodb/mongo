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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_push.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using std::vector;

REGISTER_ACCUMULATOR(push, genericParseSingleExpressionAccumulator<AccumulatorPush>);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(push, AccumulatorPush, WindowFunctionPush);

void AccumulatorPush::processInternal(const Value& input, bool merging) {
    if (!merging) {
        if (!input.missing()) {
            _array.push_back(input);
            _memUsageTracker.add(input.getApproximateSize());
            uassert(ErrorCodes::ExceededMemoryLimit,
                    str::stream()
                        << "$push used too much memory and cannot spill to disk. Memory limit: "
                        << _memUsageTracker.maxAllowedMemoryUsageBytes() << " bytes",
                    _memUsageTracker.withinMemoryLimit());
        }
    } else {
        // If we're merging, we need to take apart the arrays we receive and put their elements into
        // the array we are collecting.  If we didn't, then we'd get an array of arrays, with one
        // array from each merge source.
        assertMergingInputType(input, BSONType::array);

        const vector<Value>& vec = input.getArray();
        for (auto&& val : vec) {
            _memUsageTracker.add(val.getApproximateSize());
            uassert(ErrorCodes::ExceededMemoryLimit,
                    str::stream()
                        << "$push used too much memory and cannot spill to disk. Memory limit: "
                        << _memUsageTracker.maxAllowedMemoryUsageBytes() << " bytes",
                    _memUsageTracker.withinMemoryLimit());
        }
        _array.insert(_array.end(), vec.begin(), vec.end());
    }
}

Value AccumulatorPush::getValue(bool toBeMerged) {
    return Value(_array);
}

AccumulatorPush::AccumulatorPush(ExpressionContext* const expCtx,
                                 boost::optional<int> maxMemoryUsageBytes)
    : AccumulatorState(expCtx, maxMemoryUsageBytes.value_or(internalQueryMaxPushBytes.load())) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorPush::reset() {
    vector<Value>().swap(_array);
    _memUsageTracker.set(sizeof(*this));
}
}  // namespace mongo
