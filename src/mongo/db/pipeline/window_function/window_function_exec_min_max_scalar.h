/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_exec.h"

namespace mongo {

/**
 * Custom WindowFunctionExec class specifically for the implementation of $minMaxScalar
 * for WindowBounds that do not remove documents from the window.
 * $minMaxScalar cannot use the generic WindowFunctionExec classes that handle non-removable
 * implementations because it cannot implement a generic accumulator, that does not allow
 * for reading the value of the "current" document.
 */
class WindowFunctionExecMinMaxScalarNonRemovable final : public WindowFunctionExec {
public:
    WindowFunctionExecMinMaxScalarNonRemovable(PartitionIterator* iter,
                                               boost::intrusive_ptr<Expression> input,
                                               std::pair<Value, Value> sMinAndsMax,
                                               MemoryUsageTracker::Impl* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kEndpoints),
                             memTracker),
          _input(input),
          _sMinAndsMax(sMinAndsMax) {}

    // TODO: SERVER-95229 fill in implemenation to support non-removable windows
    Value getNext(boost::optional<Document> current) override {
        uasserted(ErrorCodes::NotImplemented,
                  "non-removable $minMaxScalar window functions are not yet supported");
        tassert(9459905,
                "WindowFunctionExecMinMaxScalarNonRemovable must be provided with the value of the "
                "current document",
                current.has_value());
        return Value{BSONNULL};
    }

    void reset() override {}

private:
    boost::intrusive_ptr<Expression> _input;
    // Output domain Value is bounded between sMin and sMax (inclusive).
    // First value is min, second value is max.
    std::pair<Value, Value> _sMinAndsMax;
};

}  // namespace mongo
