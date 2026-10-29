// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * For each document with a null value, $linearFill uses the difference on the sortBy field to
 * calculate the percentage of the missing value range that should be covered by this document, and
 * fill each document proportionally.
 */
class WindowFunctionExecLinearFill final : public WindowFunctionExec {
public:
    static inline const Value kDefault = Value(BSONNULL);

    WindowFunctionExecLinearFill(PartitionIterator* iter,
                                 boost::intrusive_ptr<Expression> input,
                                 boost::intrusive_ptr<ExpressionFieldPath> sortBy,
                                 WindowBounds bounds,
                                 SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kManual),
                             memTracker),
          _input(std::move(input)),
          _sortBy(std::move(sortBy)),
          _bounds(std::move(bounds)),
          _prevX1Y1(boost::none),
          _prevX2Y2(boost::none) {}
    Value getNext(boost::optional<Document> current = boost::none) final;
    void reset() final {
        _prevX1Y1 = boost::none;
        _prevX2Y2 = boost::none;
        _lastSeenElement = Value();
    }

private:
    // The $linearFill Window Function Executor receives sorted pairs of values for each doc, the
    // sortBy field value and fill field value. This class refers to the sortBy value as X and the
    // fill value as Y. In this way, (X1, Y1) refers the first known pair and (X2, Y2) refers to the
    // second known pair.
    boost::optional<std::pair<Value, Value>> findX2Y2();
    boost::optional<Value> evaluateInput(const Document& doc);
    boost::intrusive_ptr<Expression> _input;
    boost::intrusive_ptr<ExpressionFieldPath> _sortBy;
    WindowBounds _bounds;
    Value _lastSeenElement;
    boost::optional<std::pair<Value, Value>> _prevX1Y1;
    boost::optional<std::pair<Value, Value>> _prevX2Y2;
};

}  // namespace mongo
