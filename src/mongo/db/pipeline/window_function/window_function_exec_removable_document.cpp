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

#include "mongo/db/pipeline/window_function/window_function_exec_removable_document.h"

namespace mongo {

WindowFunctionExecRemovableDocument::WindowFunctionExecRemovableDocument(
    PartitionIterator* iter,
    boost::intrusive_ptr<Expression> input,
    std::unique_ptr<WindowFunctionState> function,
    WindowBounds::DocumentBased bounds,
    MemoryUsageTracker::PerFunctionMemoryTracker* memTracker)
    : WindowFunctionExecRemovable(iter,
                                  PartitionAccessor::Policy::kDefaultSequential,
                                  std::move(input),
                                  std::move(function),

                                  memTracker) {
    stdx::visit(
        OverloadedVisitor{
            [](const WindowBounds::Unbounded&) {
                // If the window is left unbounded we should use the non-removable executor.
                MONGO_UNREACHABLE_TASSERT(5339802);
            },
            [&](const WindowBounds::Current&) { _lowerBound = 0; },
            [&](const int& lowerIndex) { _lowerBound = lowerIndex; },
        },
        bounds.lower);

    stdx::visit(
        OverloadedVisitor{
            [](const WindowBounds::Unbounded&) {
                // Pass. _upperBound defaults to boost::none which represents no upper
                // bound.
            },
            [&](const WindowBounds::Current&) { _upperBound = 0; },
            [&](const int& upperIndex) { _upperBound = upperIndex; },
        },
        bounds.upper);
    _memTracker->set(sizeof(*this));
}

void WindowFunctionExecRemovableDocument::initialize() {
    int lowerBoundForInit = _lowerBound > 0 ? _lowerBound : 0;
    // Run the loop until we hit the out of partition break (right unbounded) or we hit the upper
    // bound.
    for (int i = lowerBoundForInit; !_upperBound || i <= _upperBound.get(); ++i) {
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
        if (auto doc = (this->_iter)[_upperBound.get()]) {
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
