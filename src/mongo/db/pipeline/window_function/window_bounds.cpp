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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/window_function/window_function_expression.h"

using boost::intrusive_ptr;
using boost::optional;

namespace mongo {

namespace {
template <class T>
WindowBounds::Bound<T> parseBound(ExpressionContext* expCtx,
                                  BSONElement elem,
                                  std::function<T(Value)> handleExpression) {
    if (elem.type() == BSONType::String) {
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
Value serializeBound(const WindowBounds::Bound<T>& bound) {
    return stdx::visit(
        OverloadedVisitor{
            [](const WindowBounds::Unbounded&) { return Value(WindowBounds::kValUnbounded); },
            [](const WindowBounds::Current&) { return Value(WindowBounds::kValCurrent); },
            [](const T& n) { return Value(n); },
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
        if (stdx::holds_alternative<WindowBounds::Current>(bound)) {
            bound = T(0);
        }
    };
    normalize(lower);
    normalize(upper);
    if (stdx::holds_alternative<T>(lower) && stdx::holds_alternative<T>(upper)) {
        T lowerVal = stdx::get<T>(lower);
        T upperVal = stdx::get<T>(upper);
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
        return stdx::holds_alternative<WindowBounds::Unbounded>(bounds.lower) &&
            stdx::holds_alternative<WindowBounds::Unbounded>(bounds.upper);
    };
    return stdx::visit(unbounded, bounds);
}

WindowBounds WindowBounds::parse(BSONObj args,
                                 const boost::optional<SortPattern>& sortBy,
                                 ExpressionContext* expCtx) {
    auto documents = args[kArgDocuments];
    auto range = args[kArgRange];
    auto unit = args[kArgUnit];

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
                args.nFields() == 0);
        return defaultBounds();
    }

    auto unpack = [](BSONElement e) -> std::pair<BSONElement, BSONElement> {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Window bounds must be a 2-element array: " << e,
                e.type() == BSONType::Array && e.Obj().nFields() == 2);
        auto lower = e.Obj()[0];
        auto upper = e.Obj()[1];
        return {lower, upper};
    };

    if (documents) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'window' field that specifies " << kArgDocuments
                              << " cannot have other fields",
                args.nFields() == 1);
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
                    args.nFields() == 2);
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "'" << kArgUnit << "' must be a string",
                    unit.type() == BSONType::String);

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
                    args.nFields() == 1);
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
        return bounds;
    }
}
void WindowBounds::serialize(MutableDocument& args) const {
    stdx::visit(
        OverloadedVisitor{
            [&](const DocumentBased& docBounds) {
                args[kArgDocuments] = Value{std::vector<Value>{
                    serializeBound(docBounds.lower),
                    serializeBound(docBounds.upper),
                }};
            },
            [&](const RangeBased& rangeBounds) {
                args[kArgRange] = Value{std::vector<Value>{
                    serializeBound(rangeBounds.lower),
                    serializeBound(rangeBounds.upper),
                }};
                if (rangeBounds.unit) {
                    args[kArgUnit] = Value{serializeTimeUnit(*rangeBounds.unit)};
                }
            },
        },
        bounds);
}
}  // namespace mongo
