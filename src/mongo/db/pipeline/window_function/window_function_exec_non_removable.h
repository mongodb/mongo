// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function_exec_non_removable_common.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * An executor that specifically handles document-based window types which only
 * accumulates values, and does not remove old ones.
 *
 * Uses a generic accumulator type (AccumulatorState), provided as a construction argument,
 * to determine how the window is updated, and reset; and how to get the current window value,
 * and fetch the current mem usage.
 */
class WindowFunctionExecNonRemovable final : public WindowFunctionExecNonRemovableCommon {
public:
    WindowFunctionExecNonRemovable(PartitionIterator* iter,
                                   boost::intrusive_ptr<Expression> input,
                                   boost::intrusive_ptr<AccumulatorState> function,
                                   WindowBounds::Bound<int> upperDocumentBound,
                                   SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExecNonRemovableCommon(iter, input, upperDocumentBound, memTracker),
          _function(std::move(function)) {};

    void updateWindow(const Value& input) final {
        _function->process(input, false);
    }

    void resetWindow() final {
        _function->reset();
    }

    Value getWindowValue(boost::optional<Document> current) final {
        return _function->getValue(false);
    }

    int64_t getMemUsage() final {
        return _function->getMemUsage();
    }

private:
    boost::intrusive_ptr<AccumulatorState> _function;
};

}  // namespace mongo
