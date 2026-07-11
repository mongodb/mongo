// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/db/pipeline/accumulator_percentile_enum_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/percentile_algo_continuous.h"
#include "mongo/db/pipeline/percentile_algo_discrete.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/util/modules.h"

#include <boost/container/flat_set.hpp>

namespace mongo {

/**
 * Shared base class for implementing $percentile and $median window functions.
 */
class WindowFunctionPercentileCommon : public WindowFunctionState {
public:
    void add(Value value) override;

    void remove(Value value) override;

    void reset() override;

protected:
    explicit WindowFunctionPercentileCommon(ExpressionContext* const expCtx,
                                            PercentileMethodEnum method)
        : WindowFunctionState(expCtx),
          _values(boost::container::flat_multiset<double>()),
          _method(method) {}

    Value computePercentile(double p) const;

    // Holds all the values in the window in ascending order.
    // A boost::container::flat_multiset stores elements in a contiguous array, so iterating through
    // the set is faster than iterating through a std::multiset which stores its elements typically
    // as a binary search tree. Thus, using a boost::container::flat_multiset significantly improved
    // performance.
    boost::container::flat_multiset<double> _values;
    PercentileMethodEnum _method;
};

class WindowFunctionPercentile : public WindowFunctionPercentileCommon {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       PercentileMethodEnum method,
                                                       const std::vector<double>& ps) {
        return std::make_unique<WindowFunctionPercentile>(expCtx, method, ps);
    }

    explicit WindowFunctionPercentile(ExpressionContext* const expCtx,
                                      PercentileMethodEnum method,
                                      const std::vector<double>& ps)
        : WindowFunctionPercentileCommon(expCtx, method), _ps(ps) {
        _memUsageTracker.set(sizeof(*this) + _ps.capacity() * sizeof(double));
    }

    Value getValue(boost::optional<Value> current = boost::none) const final;

    void reset() final;

private:
    std::vector<double> _ps;
};

class WindowFunctionMedian : public WindowFunctionPercentileCommon {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       PercentileMethodEnum method) {
        return std::make_unique<WindowFunctionMedian>(expCtx, method);
    }

    explicit WindowFunctionMedian(ExpressionContext* const expCtx, PercentileMethodEnum method)
        : WindowFunctionPercentileCommon(expCtx, method) {
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current = boost::none) const final;

    void reset() final;
};

}  // namespace mongo
