// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_limit.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_push.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

using std::vector;

REGISTER_ACCUMULATOR(push, genericParseSingleExpressionAccumulator<AccumulatorPush>);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(push, AccumulatorPush, WindowFunctionPush);

void AccumulatorPush::processInternal(const Value& input, bool merging) {
    if (!merging) {
        if (!input.missing()) {
            _array.push_back(input);
            _memUsageTracker.add(input.getApproximateSize());
            checkMemUsage();
        }
    } else {
        // If we're merging, we need to take apart the arrays we receive and put their elements into
        // the array we are collecting.  If we didn't, then we'd get an array of arrays, with one
        // array from each merge source.
        assertMergingInputType(input, BSONType::array);

        const vector<Value>& vec = input.getArray();
        for (auto&& val : vec) {
            _memUsageTracker.add(val.getApproximateSize());
            checkMemUsage();
        }
        _array.insert(_array.end(), vec.begin(), vec.end());
    }
}

Value AccumulatorPush::getValue(bool toBeMerged) {
    return Value(_array);
}

AccumulatorPush::AccumulatorPush(ExpressionContext* const expCtx,
                                 boost::optional<MemoryUsageLimit> maxMemoryUsageBytes)
    : AccumulatorState(expCtx,
                       maxMemoryUsageBytes.value_or(MemoryUsageLimit{query_knobs::kMaxPushBytes})) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorPush::reset() {
    vector<Value>().swap(_array);
    _memUsageTracker.set(sizeof(*this));
}
}  // namespace mongo
