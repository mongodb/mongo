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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/query/datetime/date_time_support.h"

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
                                 MemoryUsageTracker::PerFunctionMemoryTracker* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kEndpoints),
                             memTracker),
          _position(std::move(position)),
          _time(std::move(time)),
          _bounds(std::move(bounds)),
          _unitMillis([&]() -> boost::optional<long long> {
              if (!unit)
                  return boost::none;

              auto status = timeUnitTypicalMilliseconds(*unit);
              tassert(status);
              return status.getValue();
          }()) {}

    Value getNext() final;
    void reset() final {}

private:
    boost::intrusive_ptr<Expression> _position;
    boost::intrusive_ptr<Expression> _time;
    WindowBounds _bounds;
    boost::optional<long long> _unitMillis;
};

}  // namespace mongo
