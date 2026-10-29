// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_add_to_set.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
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

REGISTER_ACCUMULATOR(addToSet, genericParseSingleExpressionAccumulator<AccumulatorAddToSet>);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(addToSet, AccumulatorAddToSet, WindowFunctionAddToSet);

void AccumulatorAddToSet::processInternal(const Value& input, bool merging) {
    auto addValue = [this](auto&& val) {
        bool inserted = _set.insert(val).second;
        if (inserted) {
            _memUsageTracker.add(val.getApproximateSize());
            checkMemUsage();
        }
    };
    if (!merging) {
        if (!input.missing()) {
            addValue(input);
        }
    } else {
        // If we're merging, we need to take apart the arrays we receive and put their elements into
        // the array we are collecting.  If we didn't, then we'd get an array of arrays, with one
        // array from each merge source.
        assertMergingInputType(input, BSONType::array);

        for (auto&& val : input.getArray()) {
            addValue(val);
        }
    }
}

Value AccumulatorAddToSet::getValue(bool toBeMerged) {
    return Value(vector<Value>(_set.begin(), _set.end()));
}

AccumulatorAddToSet::AccumulatorAddToSet(ExpressionContext* const expCtx,
                                         boost::optional<MemoryUsageLimit> maxMemoryUsageBytes)
    : AccumulatorState(
          expCtx, maxMemoryUsageBytes.value_or(MemoryUsageLimit{query_knobs::kMaxAddToSetBytes})),
      _set(expCtx->getValueComparator().makeFlatUnorderedValueSet()) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorAddToSet::reset() {
    _set = getExpressionContext()->getValueComparator().makeFlatUnorderedValueSet();
    _memUsageTracker.set(sizeof(*this));
}
}  // namespace mongo
