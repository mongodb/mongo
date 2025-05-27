/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "expression.h"

namespace mongo {

/**
 * InclusiveBoundType defines the necessary configuration for inclusively bounded trig functions.
 */
struct InclusiveBoundType {
    // We use a static method rather than a field because the value, as an std::string, would need
    // to be initialized out of line. This method will be inlined, and result in no overhead.
    static std::string leftBracket() {
        return "[";
    }

    static std::string rightBracket() {
        return "]";
    }

    static bool checkUpperBound(double input, double bound) {
        return input <= bound;
    }

    static bool checkUpperBound(Decimal128 input, double bound) {
        return input.isLessEqual(Decimal128(bound));
    }

    static bool checkLowerBound(double input, double bound) {
        return input >= bound;
    }

    static bool checkLowerBound(Decimal128 input, double bound) {
        return input.isGreaterEqual(Decimal128(bound));
    }
};

/**
 * ExclusiveBoundType defines the necessary configuration for exclusively bounded trig functions.
 */
struct ExclusiveBoundType {
    // We use a static method rather than a field because the value, as an std::string, would need
    // to be initialized out of line. This method will be inlined, and result in no overhead.
    static std::string leftBracket() {
        return "(";
    }

    static std::string rightBracket() {
        return ")";
    }

    static bool checkUpperBound(double input, double bound) {
        return input < bound;
    }

    static bool checkUpperBound(Decimal128 input, double bound) {
        return input.isLess(Decimal128(bound));
    }

    static bool checkLowerBound(double input, double bound) {
        return input > bound;
    }

    static bool checkLowerBound(Decimal128 input, double bound) {
        return input.isGreater(Decimal128(bound));
    }
};

/**
 * ExpressionBoundedTrigonometric is the type of all trigonometric functions that take one argument
 * and have lower and upper bounds, either inclusive or exclusive, as defined by the BoundType
 * template argument.
 */
template <typename BoundedTrigType, typename BoundType>
class ExpressionBoundedTrigonometric : public ExpressionSingleNumericArg<BoundedTrigType> {
public:
    explicit ExpressionBoundedTrigonometric(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<BoundedTrigType>(expCtx) {}
    explicit ExpressionBoundedTrigonometric(ExpressionContext* const expCtx,
                                            Expression::ExpressionVector&& children)
        : ExpressionSingleNumericArg<BoundedTrigType>(expCtx, std::move(children)) {}

    std::string toString(double d) const {
        return str::stream() << d;
    }

    std::string toString(Decimal128 d) const {
        return d.toString();
    }

    template <typename T>
    bool checkBounds(T input) const {
        return BoundType::checkLowerBound(input, getLowerBound()) &&
            BoundType::checkUpperBound(input, getUpperBound());
    }

    /**
     * assertBounds uasserts if checkBounds returns false, meaning that the input is out of bounds.
     */
    template <typename T>
    void assertBounds(T input) const {
        uassert(50989,
                str::stream() << "cannot apply " << getOpName() << " to " << toString(input)
                              << ", value must be in " << BoundType::leftBracket()
                              << getLowerBound() << "," << getUpperBound()
                              << BoundType::rightBracket(),
                checkBounds(input));
    }

    /**
     * Since bounds are always either +/-Infinity or integral values, double has enough precision.
     */
    virtual double getLowerBound() const = 0;
    virtual double getUpperBound() const = 0;
    /**
     * getOpName returns the name of the operation, e.g., $sin
     */
    const char* getOpName() const override = 0;
};

/**
 * ExpressionUnboundedTrigonometric is the type for all trigonometric functions that do not have
 * upper or lower bounds.
 */
template <typename TrigType>
class ExpressionUnboundedTrigonometric : public ExpressionSingleNumericArg<TrigType> {
public:
    explicit ExpressionUnboundedTrigonometric(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg<TrigType>(expCtx) {}
    explicit ExpressionUnboundedTrigonometric(ExpressionContext* const expCtx,
                                              Expression::ExpressionVector&& children)
        : ExpressionSingleNumericArg<TrigType>(expCtx, std::move(children)) {}

    /**
     * getOpName returns the name of the operation, e.g., $sinh
     */
    const char* getOpName() const override = 0;
};

class ExpressionArcTangent2 final : public ExpressionTwoNumericArgs<ExpressionArcTangent2> {
public:
    explicit ExpressionArcTangent2(ExpressionContext* const expCtx)
        : ExpressionTwoNumericArgs(expCtx) {}

    ExpressionArcTangent2(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionTwoNumericArgs(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const final {
        return "$atan2";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


/* ----------------------- Inclusive Bounded Trigonometric Functions ---------------------------- */

#define CREATE_BOUNDED_TRIGONOMETRIC_CLASS(className, funcName, boundType, lowerBound, upperBound) \
    class Expression##className final                                                              \
        : public ExpressionBoundedTrigonometric<Expression##className, boundType> {                \
    public:                                                                                        \
        explicit Expression##className(ExpressionContext* const expCtx)                            \
            : ExpressionBoundedTrigonometric(expCtx) {}                                            \
        explicit Expression##className(ExpressionContext* const expCtx,                            \
                                       ExpressionVector&& children)                                \
            : ExpressionBoundedTrigonometric(expCtx, std::move(children)) {}                       \
                                                                                                   \
        Value evaluate(const Document& root, Variables* variables) const final;                    \
                                                                                                   \
        double getLowerBound() const final {                                                       \
            return lowerBound;                                                                     \
        }                                                                                          \
                                                                                                   \
        double getUpperBound() const final {                                                       \
            return upperBound;                                                                     \
        }                                                                                          \
                                                                                                   \
        const char* getOpName() const final {                                                      \
            return "$" #funcName;                                                                  \
        }                                                                                          \
                                                                                                   \
        void acceptVisitor(ExpressionMutableVisitor* visitor) final {                              \
            return visitor->visit(this);                                                           \
        }                                                                                          \
                                                                                                   \
        void acceptVisitor(ExpressionConstVisitor* visitor) const final {                          \
            return visitor->visit(this);                                                           \
        }                                                                                          \
    };


/**
 * Inclusive Bounds
 */
CREATE_BOUNDED_TRIGONOMETRIC_CLASS(ArcCosine, acos, InclusiveBoundType, -1.0, 1.0);

CREATE_BOUNDED_TRIGONOMETRIC_CLASS(ArcSine, asin, InclusiveBoundType, -1.0, 1.0);

CREATE_BOUNDED_TRIGONOMETRIC_CLASS(HyperbolicArcTangent, atanh, InclusiveBoundType, -1.0, 1.0);

CREATE_BOUNDED_TRIGONOMETRIC_CLASS(
    HyperbolicArcCosine, acosh, InclusiveBoundType, 1.0, std::numeric_limits<double>::infinity());

/**
 * Exclusive Bounds
 */
CREATE_BOUNDED_TRIGONOMETRIC_CLASS(Cosine,
                                   cos,
                                   ExclusiveBoundType,
                                   -std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::infinity());

CREATE_BOUNDED_TRIGONOMETRIC_CLASS(Sine,
                                   sin,
                                   ExclusiveBoundType,
                                   -std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::infinity());

CREATE_BOUNDED_TRIGONOMETRIC_CLASS(Tangent,
                                   tan,
                                   ExclusiveBoundType,
                                   -std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::infinity());

#undef CREATE_BOUNDED_TRIGONOMETRIC_CLASS

/* ----------------------- Unbounded Trigonometric Functions ---------------------------- */


#define CREATE_TRIGONOMETRIC_CLASS(className, funcName)                         \
    class Expression##className final                                           \
        : public ExpressionUnboundedTrigonometric<Expression##className> {      \
    public:                                                                     \
        explicit Expression##className(ExpressionContext* const expCtx)         \
            : ExpressionUnboundedTrigonometric(expCtx) {}                       \
        explicit Expression##className(ExpressionContext* const expCtx,         \
                                       ExpressionVector&& children)             \
            : ExpressionUnboundedTrigonometric(expCtx, std::move(children)) {}  \
                                                                                \
        Value evaluate(const Document& root, Variables* variables) const final; \
                                                                                \
        const char* getOpName() const final {                                   \
            return "$" #funcName;                                               \
        }                                                                       \
                                                                                \
        void acceptVisitor(ExpressionMutableVisitor* visitor) final {           \
            return visitor->visit(this);                                        \
        }                                                                       \
                                                                                \
        void acceptVisitor(ExpressionConstVisitor* visitor) const final {       \
            return visitor->visit(this);                                        \
        }                                                                       \
    };

CREATE_TRIGONOMETRIC_CLASS(ArcTangent, atan);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicArcSine, asinh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicCosine, cosh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicSine, sinh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicTangent, tanh);

#undef CREATE_TRIGONOMETRIC_CLASS

/* ----------------------- ExpressionDegreesToRadians and ExpressionRadiansToDegrees ---- */

class ExpressionDegreesToRadians final
    : public ExpressionSingleNumericArg<ExpressionDegreesToRadians> {
public:
    explicit ExpressionDegreesToRadians(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg(expCtx) {}
    explicit ExpressionDegreesToRadians(ExpressionContext* const expCtx,
                                        ExpressionVector&& children)
        : ExpressionSingleNumericArg(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const final {
        return "$degreesToRadians";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionRadiansToDegrees final
    : public ExpressionSingleNumericArg<ExpressionRadiansToDegrees> {
public:
    explicit ExpressionRadiansToDegrees(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg(expCtx) {}
    explicit ExpressionRadiansToDegrees(ExpressionContext* const expCtx,
                                        ExpressionVector&& children)
        : ExpressionSingleNumericArg(expCtx, std::move(children)) {}

    Value evaluate(const Document& root, Variables* variables) const final;

    const char* getOpName() const final {
        return "$radiansToDegrees";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};


}  // namespace mongo
