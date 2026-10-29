// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_exec_removable_document.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"

#include <cstdlib>
#include <utility>
#include <variant>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

WindowFunctionExecRemovableDocument::WindowFunctionExecRemovableDocument(
    PartitionIterator* iter,
    boost::intrusive_ptr<Expression> input,
    std::unique_ptr<WindowFunctionState> function,
    WindowBounds::DocumentBased bounds,
    SimpleMemoryUsageTracker* memTracker)
    : WindowFunctionExecRemovable(iter,
                                  PartitionAccessor::Policy::kDefaultSequential,
                                  std::move(input),
                                  std::move(function),

                                  memTracker) {
    visit(OverloadedVisitor{
              [](const WindowBounds::Unbounded&) {
                  // If the window is left unbounded we should use the non-removable executor.
                  MONGO_UNREACHABLE_TASSERT(5339802);
              },
              [&](const WindowBounds::Current&) { _lowerBound = 0; },
              [&](const int& lowerIndex) { _lowerBound = lowerIndex; },
          },
          bounds.lower);

    visit(OverloadedVisitor{
              [](const WindowBounds::Unbounded&) {
                  // Pass. _upperBound defaults to boost::none which represents no upper
                  // bound.
              },
              [&](const WindowBounds::Current&) { _upperBound = 0; },
              [&](const int& upperIndex) { _upperBound = upperIndex; },
          },
          bounds.upper);
}

void WindowFunctionExecRemovableDocument::initialize() {
    int lowerBoundForInit = _lowerBound > 0 ? _lowerBound : 0;
    // Run the loop until we hit the out of partition break (right unbounded) or we hit the upper
    // bound.
    for (int i = lowerBoundForInit; !_upperBound || i <= _upperBound.value(); ++i) {
        // If this is false, we're over the end of the partition.
        if (auto doc = (this->_iter)[i]) {
            addValue(_input->evaluate(*doc, &_input->getExpressionContext()->variables));
        } else {
            break;
        }
    }
    _initialized = true;
}

void WindowFunctionExecRemovableDocument::update() {
    if (!_initialized) {
        initialize();
        return;
    }

    // If there is no upper bound, the whole partition is loaded by initialize.
    if (_upperBound) {
        // If this is false, we're over the end of the partition.
        if (auto doc = (this->_iter)[_upperBound.value()]) {
            addValue(_input->evaluate(*doc, &_input->getExpressionContext()->variables));
        }
    }

    // For a positive lower bound the first pass loads the correct window, so subsequent passes
    // must always remove a document if there is a document left to remove.
    // For a negative lower bound we can start removing every time only after we have seen
    // documents to fill the left side of the window.
    if (_lowerBound >= 0 || _iter.getCurrentPartitionIndex() > abs(_lowerBound)) {
        removeFirstValueIfExists();
    }
}

}  // namespace mongo
