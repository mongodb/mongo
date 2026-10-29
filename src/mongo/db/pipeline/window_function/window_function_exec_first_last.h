// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/modules.h"

#include <utility>

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
