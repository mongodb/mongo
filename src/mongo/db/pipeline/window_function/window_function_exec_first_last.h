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
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class WindowFunctionExecForEndpoint : public WindowFunctionExec {
protected:
    WindowFunctionExecForEndpoint(PartitionIterator* iter,
                                  boost::intrusive_ptr<Expression> input,
                                  WindowBounds bounds,
                                  const boost::optional<Value>& defaultValue,
                                  SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kEndpoints),
                             memTracker),
          _input(std::move(input)),
          _bounds(std::move(bounds)),
          _default(defaultValue.get_value_or(Value(BSONNULL))) {}

    Value getFirst() {
        auto endpoints = _iter.getEndpoints(_bounds);
        if (!endpoints) {
            return _default;
        }
        const Document doc = *(_iter)[endpoints->first];
        auto result = _input->evaluate(doc, &_input->getExpressionContext()->variables);
        if (result.missing()) {
            result = _default;
        }
        return result;
    }

    Value getLast() {
        auto endpoints = _iter.getEndpoints(_bounds);
        if (!endpoints) {
            return _default;
        }
        const Document doc = *(_iter)[endpoints->second];
        auto result = _input->evaluate(doc, &_input->getExpressionContext()->variables);
        if (result.missing()) {
            result = _default;
        }
        return result;
    }

    void reset() final {}

private:
    boost::intrusive_ptr<Expression> _input;
    WindowBounds _bounds;
    Value _default;
};

class WindowFunctionExecFirst final : public WindowFunctionExecForEndpoint {
public:
    WindowFunctionExecFirst(PartitionIterator* iter,
                            boost::intrusive_ptr<Expression> input,
                            WindowBounds bounds,
                            const boost::optional<Value>& defaultValue,
                            SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExecForEndpoint(
              iter, std::move(input), std::move(bounds), defaultValue, memTracker) {}

    Value getNext(boost::optional<Document> current = boost::none) override {
        return getFirst();
    }
};

class WindowFunctionExecLast final : public WindowFunctionExecForEndpoint {
public:
    WindowFunctionExecLast(PartitionIterator* iter,
                           boost::intrusive_ptr<Expression> input,
                           WindowBounds bounds,
                           SimpleMemoryUsageTracker* memTracker)
        : WindowFunctionExecForEndpoint(
              iter, std::move(input), std::move(bounds), boost::none, memTracker) {}

    Value getNext(boost::optional<Document> current = boost::none) override {
        return getLast();
    }
};

}  // namespace mongo
