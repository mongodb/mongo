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
#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"

namespace mongo {

class WindowFunctionExecForEndpoint : public WindowFunctionExec {
public:
    // Endpoint executors are constant size and don't hold any of the values passing through.
    size_t getApproximateSize() const final {
        return 0;
    }

protected:
    WindowFunctionExecForEndpoint(PartitionIterator* iter,
                                  boost::intrusive_ptr<Expression> input,
                                  WindowBounds bounds,
                                  boost::optional<boost::intrusive_ptr<Expression>> defaultValue)
        : WindowFunctionExec(PartitionAccessor(iter, PartitionAccessor::Policy::kEndpoints)),
          _input(std::move(input)),
          _bounds(std::move(bounds)) {
        if (!defaultValue) {
            _default = Value{BSONNULL};
        } else {
            boost::intrusive_ptr<Expression> expr = (*defaultValue)->optimize();
            ExpressionConstant* ec = dynamic_cast<ExpressionConstant*>(expr.get());
            tassert(ErrorCodes::FailedToParse, "default expression must be a constant.", ec);
            _default = ec->getValue();
        }
    }

    Value getFirst() {
        auto endpoints = _iter.getEndpoints(_bounds);
        if (!endpoints)
            return _default;
        const Document doc = *(_iter)[endpoints->first];
        return _input->evaluate(doc, &_input->getExpressionContext()->variables);
    }

    Value getLast() {
        auto endpoints = _iter.getEndpoints(_bounds);
        if (!endpoints)
            return _default;
        const Document doc = *(_iter)[endpoints->second];
        return _input->evaluate(doc, &_input->getExpressionContext()->variables);
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
                            boost::optional<boost::intrusive_ptr<Expression>> defaultValue)
        : WindowFunctionExecForEndpoint(
              iter, std::move(input), std::move(bounds), std::move(defaultValue)) {}

    Value getNext() {
        return getFirst();
    }
};

class WindowFunctionExecLast final : public WindowFunctionExecForEndpoint {
public:
    WindowFunctionExecLast(PartitionIterator* iter,
                           boost::intrusive_ptr<Expression> input,
                           WindowBounds bounds)
        : WindowFunctionExecForEndpoint(iter, input, std::move(bounds), boost::none) {}

    Value getNext() {
        return getLast();
    }
};

}  // namespace mongo
