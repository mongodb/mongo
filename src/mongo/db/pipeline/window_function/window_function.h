// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * A WindowFunctionState is a mutable, removable accumulator.
 *
 * Implementations must ensure that 'remove()' undoes 'add()' when called in FIFO order.
 * For example:
 *     'add(x); add(y); remove(x)' == 'add(y)'
 *     'add(a); add(b); add(z); remove(a); remove(b)' == 'add(z)'
 */
class WindowFunctionState {
public:
    WindowFunctionState(ExpressionContext* const expCtx,
                        MemoryUsageLimit maxAllowedMemoryUsageBytes =
                            MemoryUsageLimit{std::numeric_limits<int64_t>::max()})
        : _expCtx(expCtx), _memUsageTracker(maxAllowedMemoryUsageBytes) {}
    virtual ~WindowFunctionState() = default;

    WindowFunctionState(const WindowFunctionState&) = delete;
    WindowFunctionState& operator=(const WindowFunctionState&) = delete;

    virtual void add(Value) = 0;
    virtual void remove(Value) = 0;
    /**
     * @param current The value of the current document whose output field Value
     * is being computed.
     * Some window functions implementations do not need the input Value of the current document
     * and expect to be able to be called without it.
     * Other window functions require this input Value and can assert this value present to proceed.
     */
    virtual Value getValue(boost::optional<Value> current = boost::none) const = 0;
    virtual void reset() = 0;
    size_t getApproximateSize() {
        tassert(5414200,
                "_memUsageTracker is not set for function",
                _memUsageTracker.inUseTrackedMemoryBytes() != 0);
        return _memUsageTracker.inUseTrackedMemoryBytes();
    }

protected:
    ExpressionContext* _expCtx;
    SimpleMemoryUsageTracker _memUsageTracker;
};
}  // namespace mongo
