// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * $derivative computes 'rise/run', by comparing the two endpoints of its window.
 *
 * 'rise' is the difference in 'position' between the endpoints; 'run' is the difference in 'time'.
 *
 * We assume the 'time' is provided as an expression, even though the surface syntax uses a
 * SortPattern. When the WindowFunctionExpression translates itself to an exec, it can also
 * translate the SortPattern to an expression.
 */
class WindowFunctionExecDerivative final : public WindowFunctionExec {
public:
    // Default value to use when the window is empty.
    static inline const Value kDefault = Value(BSONNULL);

    WindowFunctionExecDerivative(PartitionIterator* iter,
                                 boost::intrusive_ptr<Expression> position,
                                 boost::intrusive_ptr<Expression> time,
                                 WindowBounds bounds,
                                 boost::optional<TimeUnit> unit,
                                 SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kEndpoints),
                             memTracker),
          _position(std::move(position)),
          _time(std::move(time)),
          _bounds(std::move(bounds)),
          _unitMillis([&]() -> boost::optional<long long> {
              if (!unit)
                  return boost::none;

              auto milliseconds = timeUnitTypicalMilliseconds(*unit);
              tassert(7823403,
                      "TimeUnit must be less than or equal to a 'week' ",
                      milliseconds <= timeUnitTypicalMilliseconds(TimeUnit::week));
              return milliseconds;
          }()) {}

    Value getNext(boost::optional<Document> current = boost::none) final;
    void reset() final {}

private:
    boost::intrusive_ptr<Expression> _position;
    boost::intrusive_ptr<Expression> _time;
    WindowBounds _bounds;
    boost::optional<long long> _unitMillis;
};

}  // namespace mongo
