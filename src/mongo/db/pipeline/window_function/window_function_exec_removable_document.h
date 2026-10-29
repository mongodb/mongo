// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/util/modules.h"

#include <memory>
#include <queue>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * An executor that specifically handles document-based window types which accumulate values while
 * removing old ones.
 */
class WindowFunctionExecRemovableDocument final : public WindowFunctionExecRemovable {
public:
    /**
     * Constructs a removable window function executor with the given input expression to be
     * evaluated and passed to the corresponding WindowFunc for each document in the window.
     *
     * The 'bounds' parameter is the user supplied bounds for the window.
     */
    WindowFunctionExecRemovableDocument(PartitionIterator* iter,
                                        boost::intrusive_ptr<Expression> input,
                                        std::unique_ptr<WindowFunctionState> function,
                                        WindowBounds::DocumentBased bounds,
                                        SimpleMemoryUsageTracker* memTracker);

private:
    void update() final;
    void initialize();

    void doReset() final {
        _initialized = false;
    }

    void removeFirstValueIfExists() {
        if (_values.size() == 0) {
            return;
        }
        removeValue();
    }

    // In one of two states: either the initial window has not been populated or we are sliding and
    // accumulating/removing values.
    bool _initialized = false;

    int _lowerBound = 0;
    // Will stay boost::none if right unbounded.
    boost::optional<int> _upperBound = boost::none;
};
}  // namespace mongo
