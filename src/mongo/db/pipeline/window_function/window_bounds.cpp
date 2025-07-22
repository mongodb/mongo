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


#include "mongo/db/pipeline/window_function/window_bounds.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <functional>
#include <utility>
#include <variant>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::optional;

namespace mongo {

namespace {
template <class T>
WindowBounds::Bound<T> parseBound(ExpressionContext* expCtx,
                                  BSONElement elem,
                                  std::function<T(Value)> handleExpression) {
    if (elem.type() == BSONType::string) {
        auto s = elem.str();
        if (s == WindowBounds::kValUnbounded) {
            return WindowBounds::Unbounded{};
        } else if (s == WindowBounds::kValCurrent) {
            return WindowBounds::Current{};
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      "Window bounds must be 'unbounded', 'current', or a number.");
        }
    } else {
        // Expect a constant number expression.
        auto expr = Expression::parseOperand(expCtx, elem, expCtx->variablesParseState);
        expr = expr->optimize();
        auto constant = dynamic_cast<ExpressionConstant*>(expr.get());
        uassert(
            ErrorCodes::FailedToParse, "Window bounds expression must be a constant.", constant);
        return handleExpression(constant->getValue());
    }
}

template <class T>
Value serializeBound(const WindowBounds::Bound<T>& bound,
                     const SerializationOptions& opts,
                     const Value& representativeValue) {
    return visit(
        OverloadedVisitor{
            [&](const WindowBounds::Unbounded&) { return Value(WindowBounds::kValUnbounded); },
            [&](const WindowBounds::Current&) { return Value(WindowBounds::kValCurrent); },
            [&](const T& n) {
                // If not "unbounded" or "current", n must be a literal constant
                // The upper bound must be greater than the lower bound. We override the
                // representative value to meet this constraint.
                return opts.serializeLiteral(n, representativeValue);
            },
        },
        bound);
}

/**
 * Make sure the bounds are oriented correctly: with lower <= upper.
 */
template <class T, class F>
void checkBoundsForward(WindowBounds::Bound<T> lower, WindowBounds::Bound<T> upper, F lessOrEqual) {
    // First normalize by treating 'current' as 0.
    // Then, if both bounds are numeric, require lower <= upper.
    auto normalize = [](WindowBounds::Bound<T>& bound) {
        if (holds_alternative<WindowBounds::Current>(bound)) {
            bound = T(0);
        }
    };
    normalize(lower);
    normalize(upper);
    if (holds_alternative<T>(lower) && holds_alternative<T>(upper)) {
        T lowerVal = get<T>(lower);
        T upperVal = get<T>(upper);
        uassert(5339900,
                str::stream() << "Lower bound must not exceed upper bound: ["
                              << Value(lowerVal).toString() << ", " << Value(upperVal).toString()
                              << "]",
                lessOrEqual(lowerVal, upperVal));
    }
}
void checkBoundsForward(WindowBounds::Bound<int> lower, WindowBounds::Bound<int> upper) {
    return checkBoundsForward(lower, upper, [](int x, int y) -> bool { return x <= y; });
}
void checkBoundsForward(WindowBounds::Bound<Value> lower, WindowBounds::Bound<Value> upper) {
    return checkBoundsForward(
        lower, upper, [](Value x, Value y) -> bool { return ValueComparator().evaluate(x <= y); });
}
}  // namespace

bool WindowBounds::isUnbounded() const {
    auto unbounded = [](const auto& bounds) {
        return holds_alternative<WindowBounds::Unbounded>(bounds.lower) &&
            holds_alternative<WindowBounds::Unbounded>(bounds.upper);
    };
    return visit(unbounded, bounds);
}

WindowBounds WindowBounds::parse(BSONElement args,
                                 const boost::optional<SortPattern>& sortBy,
                                 ExpressionContext* expCtx) {
    uassert(ErrorCodes::FailedToParse,
            "'window' field must be an object",
            args.type() == BSONType::object);
    auto argObj = args.embeddedObject();
    auto documents = argObj[kArgDocuments];
    auto range = argObj[kArgRange];
    auto unit = argObj[kArgUnit];

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Window bounds can specify either '" << kArgDocuments << "' or '"
                          << kArgUnit << "', not both.",
            !(documents && range));
    if (!range) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Window bounds can only specify '" << kArgUnit
                              << "' with range-based bounds.",
                !unit);
    }

    if (!range && !documents) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'window' field can only contain '" << kArgDocuments
                              << "' as the only argument or '" << kArgRange
                              << "' with an optional '" << kArgUnit << "' field",
                argObj.nFields() == 0);
        return defaultBounds();
    }

    auto unpack = [](BSONElement e) -> std::pair<BSONElement, BSONElement> {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Window bounds must be a 2-element array: " << e,
                e.type() == BSONType::array && e.Obj().nFields() == 2);
        auto lower = e.Obj()[0];
        auto upper = e.Obj()[1];
        return {lower, upper};
    };

    if (documents) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'window' field that specifies " << kArgDocuments
                              << " cannot have other fields",
                argObj.nFields() == 1);
        // Parse document-based bounds.
        auto [lowerElem, upperElem] = unpack(documents);

        auto parseInt = [](Value v) -> int {
            uassert(ErrorCodes::FailedToParse,
                    "Numeric document-based bounds must be an integer",
                    v.integral());
            return v.coerceToInt();
        };
        auto lower = parseBound<int>(expCtx, lowerElem, parseInt);
        auto upper = parseBound<int>(expCtx, upperElem, parseInt);
        checkBoundsForward(lower, upper);
        auto bounds = WindowBounds{DocumentBased{lower, upper}};
        uassert(5339901, "Document-based bounds require a sortBy", bounds.isUnbounded() || sortBy);
        return bounds;
    } else {
        auto [lowerElem, upperElem] = unpack(range);
        WindowBounds bounds;
        if (unit) {
            // Parse time-based bounds (range-based, with a unit).
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "'window' field that specifies " << kArgUnit
                                  << " cannot have other fields besides 'range'",
                    argObj.nFields() == 2);
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "'" << kArgUnit << "' must be a string",
                    unit.type() == BSONType::string);

            auto parseInt = [](Value v) -> Value {
                uassert(ErrorCodes::FailedToParse,
                        str::stream()
                            << "With '" << kArgUnit << "', range-based bounds must be an integer",
                        v.integral());
                return v;
            };
            // Syntactically, time-based bounds can't be fractional. So parse as int.
            auto lower = parseBound<Value>(expCtx, lowerElem, parseInt);
            auto upper = parseBound<Value>(expCtx, upperElem, parseInt);
            checkBoundsForward(lower, upper);
            bounds = WindowBounds{RangeBased{lower, upper, parseTimeUnit(unit.str())}};
        } else {
            // Parse range-based bounds.
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "'window' field that specifies " << kArgRange
                                  << " cannot have other fields besides 'unit'",
                    argObj.nFields() == 1);
            auto parseNumber = [](Value v) -> Value {
                uassert(ErrorCodes::FailedToParse,
                        "Range-based bounds expression must be a number",
                        v.numeric());
                return v;
            };
            auto lower = parseBound<Value>(expCtx, lowerElem, parseNumber);
            auto upper = parseBound<Value>(expCtx, upperElem, parseNumber);
            checkBoundsForward(lower, upper);
            bounds = WindowBounds{RangeBased{lower, upper}};
        }
        uassert(5339902,
                "Range-based bounds require sortBy a single field",
                sortBy && sortBy->size() == 1);
        const SortPattern::SortPatternPart& part = *sortBy->begin();
        uassert(8947400,
                "Range-based bounds require a non-expression sortBy",
                part.fieldPath && !part.expression);
        uassert(8947401, "Range-based bounds require an ascending sortBy", part.isAscending);
        return bounds;
    }
}
void WindowBounds::serialize(MutableDocument& args, const SerializationOptions& opts) const {
    visit(
        OverloadedVisitor{
            [&](const DocumentBased& docBounds) {
                args[kArgDocuments] = Value{std::vector<Value>{
                    serializeBound(
                        docBounds.lower, opts, /* representative value, if needed */ Value(0LL)),
                    serializeBound(
                        docBounds.upper, opts, /* representative value, if needed */ Value(1LL)),
                }};
            },
            [&](const RangeBased& rangeBounds) {
                args[kArgRange] = Value{std::vector<Value>{
                    serializeBound(
                        rangeBounds.lower, opts, /* representative value, if needed */ Value(0LL)),
                    serializeBound(
                        rangeBounds.upper, opts, /* representative value, if needed */ Value(1LL)),
                }};
                if (rangeBounds.unit) {
                    args[kArgUnit] = Value(serializeTimeUnit(*rangeBounds.unit));
                }
            },
        },
        bounds);
}
}  // namespace mongo
