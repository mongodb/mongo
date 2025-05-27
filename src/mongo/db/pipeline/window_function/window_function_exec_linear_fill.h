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

#include <utility>

#include <boost/move/utility_core.hpp>
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
