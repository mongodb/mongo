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

    bool isnan(double d) const {
        return std::isnan(d);
    }

    bool isnan(Decimal128 d) const {
        return d.isNaN();
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
     * evaluateNumericArg  evaluates the implented trig function on one numericArg.
     */
    Value evaluateNumericArg(const Value& numericArg) const {
        switch (numericArg.getType()) {
            case BSONType::NumberDouble: {
                auto input = numericArg.getDouble();
                if (isnan(input)) {
                    return numericArg;
                }
                assertBounds(input);
                return Value(doubleFunc(input));
            }
            case BSONType::NumberDecimal: {
                auto input = numericArg.getDecimal();
                if (isnan(input)) {
                    return numericArg;
                }
                assertBounds(input);
                return Value(decimalFunc(input));
            }
            default: {
                auto input = static_cast<double>(numericArg.getLong());
                if (isnan(input)) {
                    return numericArg;
                }
                assertBounds(input);
                return Value(doubleFunc(input));
            }
        }
    }

    /**
     * Since bounds are always either +/-Infinity or integral values, double has enough precision.
     */
    virtual double getLowerBound() const = 0;
    virtual double getUpperBound() const = 0;
    /**
     * doubleFunc performs the double version of the implemented trig function, e.g. std::sin()
     */
    virtual double doubleFunc(double x) const = 0;
    /**
     * decimalFunc performs the decimal128 version of the implemented trig function, e.g. d.sin()
     */
    virtual Decimal128 decimalFunc(Decimal128 x) const = 0;
    /**
     * getOpName returns the name of the operation, e.g., $sin
     */
    virtual const char* getOpName() const = 0;
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
     * evaluateNumericArg evaluates the implented trig function on one numericArg.
     */
    Value evaluateNumericArg(const Value& numericArg) const override {
        switch (numericArg.getType()) {
            case BSONType::NumberDouble:
                return Value(doubleFunc(numericArg.getDouble()));
            case BSONType::NumberDecimal:
                return Value(decimalFunc(numericArg.getDecimal()));
            default: {
                auto num = static_cast<double>(numericArg.getLong());
                return Value(doubleFunc(num));
            }
        }
    }

    /**
     * doubleFunc performs the double version of the implemented trig function, e.g. std::sinh()
     */
    virtual double doubleFunc(double x) const = 0;
    /**
     * decimalFunc performs the decimal128 version of the implemented trig function, e.g. d.sinh()
     */
    virtual Decimal128 decimalFunc(Decimal128 x) const = 0;
    /**
     * getOpName returns the name of the operation, e.g., $sinh
     */
    virtual const char* getOpName() const = 0;
};

class ExpressionArcTangent2 final : public ExpressionTwoNumericArgs<ExpressionArcTangent2> {
public:
    explicit ExpressionArcTangent2(ExpressionContext* const expCtx)
        : ExpressionTwoNumericArgs(expCtx) {}

    ExpressionArcTangent2(ExpressionContext* const expCtx, ExpressionVector&& children)
        : ExpressionTwoNumericArgs(expCtx, std::move(children)) {}

    Value evaluateNumericArgs(const Value& numericArg1, const Value& numericArg2) const final {
        auto totalType = BSONType::NumberDouble;
        // If the type of either argument is NumberDecimal, we promote to Decimal128.
        if (numericArg1.getType() == BSONType::NumberDecimal ||
            numericArg2.getType() == BSONType::NumberDecimal) {
            totalType = BSONType::NumberDecimal;
        }
        switch (totalType) {
            case BSONType::NumberDecimal: {
                auto dec = numericArg1.coerceToDecimal();
                return Value(dec.atan2(numericArg2.coerceToDecimal()));
            }
            case BSONType::NumberDouble: {
                return Value(
                    std::atan2(numericArg1.coerceToDouble(), numericArg2.coerceToDouble()));
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    const char* getOpName() const final {
        return "$atan2";
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
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
        double getLowerBound() const final {                                                       \
            return lowerBound;                                                                     \
        }                                                                                          \
                                                                                                   \
        double getUpperBound() const final {                                                       \
            return upperBound;                                                                     \
        }                                                                                          \
                                                                                                   \
        double doubleFunc(double arg) const final {                                                \
            return std::funcName(arg);                                                             \
        }                                                                                          \
                                                                                                   \
        Decimal128 decimalFunc(Decimal128 arg) const final {                                       \
            return arg.funcName();                                                                 \
        }                                                                                          \
                                                                                                   \
        const char* getOpName() const final {                                                      \
            return "$" #funcName;                                                                  \
        }                                                                                          \
                                                                                                   \
        void acceptVisitor(ExpressionVisitor* visitor) final {                                     \
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


#define CREATE_TRIGONOMETRIC_CLASS(className, funcName)                        \
    class Expression##className final                                          \
        : public ExpressionUnboundedTrigonometric<Expression##className> {     \
    public:                                                                    \
        explicit Expression##className(ExpressionContext* const expCtx)        \
            : ExpressionUnboundedTrigonometric(expCtx) {}                      \
        explicit Expression##className(ExpressionContext* const expCtx,        \
                                       ExpressionVector&& children)            \
            : ExpressionUnboundedTrigonometric(expCtx, std::move(children)) {} \
                                                                               \
        double doubleFunc(double arg) const final {                            \
            return std::funcName(arg);                                         \
        }                                                                      \
                                                                               \
        Decimal128 decimalFunc(Decimal128 arg) const final {                   \
            return arg.funcName();                                             \
        }                                                                      \
                                                                               \
        const char* getOpName() const final {                                  \
            return "$" #funcName;                                              \
        }                                                                      \
                                                                               \
        void acceptVisitor(ExpressionVisitor* visitor) final {                 \
            return visitor->visit(this);                                       \
        }                                                                      \
    };

CREATE_TRIGONOMETRIC_CLASS(ArcTangent, atan);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicArcSine, asinh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicCosine, cosh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicSine, sinh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicTangent, tanh);

#undef CREATE_TRIGONOMETRIC_CLASS

/* ----------------------- ExpressionDegreesToRadians and ExpressionRadiansToDegrees ---- */

static constexpr double kDoublePi = 3.141592653589793;
static constexpr double kDoublePiOver180 = kDoublePi / 180.0;
static constexpr double kDouble180OverPi = 180.0 / kDoublePi;

static Value doDegreeRadiansConversion(const Value& numericArg,
                                       Decimal128 decimalFactor,
                                       double doubleFactor) {
    switch (numericArg.getType()) {
        case BSONType::NumberDecimal:
            return Value(numericArg.getDecimal().multiply(decimalFactor));
        default:
            return Value(numericArg.coerceToDouble() * doubleFactor);
    }
}

class ExpressionDegreesToRadians final
    : public ExpressionSingleNumericArg<ExpressionDegreesToRadians> {
public:
    explicit ExpressionDegreesToRadians(ExpressionContext* const expCtx)
        : ExpressionSingleNumericArg(expCtx) {}
    explicit ExpressionDegreesToRadians(ExpressionContext* const expCtx,
                                        ExpressionVector&& children)
        : ExpressionSingleNumericArg(expCtx, std::move(children)) {}

    Value evaluateNumericArg(const Value& numericArg) const final {
        return doDegreeRadiansConversion(numericArg, Decimal128::kPiOver180, kDoublePiOver180);
    }

    const char* getOpName() const final {
        return "$degreesToRadians";
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
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

    Value evaluateNumericArg(const Value& numericArg) const final {
        return doDegreeRadiansConversion(numericArg, Decimal128::k180OverPi, kDouble180OverPi);
    }

    const char* getOpName() const final {
        return "$radiansToDegrees";
    }

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }
};


}  // namespace mongo
