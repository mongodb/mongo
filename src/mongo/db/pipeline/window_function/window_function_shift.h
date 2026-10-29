// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::window_function {
using namespace std::literals::string_view_literals;

class ExpressionShift : public Expression {
public:
    static constexpr std::string_view kDefaultArg = "default"sv;
    static constexpr std::string_view kOutputArg = "output"sv;
    static constexpr std::string_view kByArg = "by"sv;

    static boost::intrusive_ptr<Expression> parse(BSONObj obj,
                                                  const boost::optional<SortPattern>& sortBy,
                                                  ExpressionContext* expCtx);


    ExpressionShift(ExpressionContext* expCtx,
                    std::string accumulatorName,
                    boost::intrusive_ptr<::mongo::Expression> output,
                    boost::optional<mongo::Value> defaultVal,
                    int offset)
        : Expression(expCtx,
                     accumulatorName,
                     std::move(output),
                     WindowBounds::documentBounds(offset, offset)),
          _defaultVal(std::move(defaultVal)),
          _offset(offset) {}

    boost::optional<mongo::Value> defaultVal() const {
        return _defaultVal;
    }

    boost::intrusive_ptr<AccumulatorState> buildAccumulatorOnly() const final {
        MONGO_UNREACHABLE_TASSERT(5424301);
    }

    std::unique_ptr<WindowFunctionState> buildRemovable() const final {
        MONGO_UNREACHABLE_TASSERT(5424302);
    }

    Value serialize(const query_shape::SerializationOptions& opts) const final;

private:
    static boost::intrusive_ptr<Expression> parseShiftArgs(BSONObj obj,
                                                           std::string_view accName,
                                                           ExpressionContext* expCtx);

    boost::optional<mongo::Value> _defaultVal;
    int _offset;
};

}  // end namespace mongo::window_function
