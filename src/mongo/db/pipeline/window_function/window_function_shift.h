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

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::window_function {

class ExpressionShift : public Expression {
public:
    static constexpr StringData kDefaultArg = "default"_sd;
    static constexpr StringData kOutputArg = "output"_sd;
    static constexpr StringData kByArg = "by"_sd;

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

    Value serialize(const SerializationOptions& opts) const final;

private:
    static boost::intrusive_ptr<Expression> parseShiftArgs(BSONObj obj,
                                                           mongo::StringData accName,
                                                           ExpressionContext* expCtx);

    boost::optional<mongo::Value> _defaultVal;
    int _offset;
};

}  // end namespace mongo::window_function
