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

#include "mongo/platform/basic.h"

#include "expression_trigonometric.h"

namespace mongo {

/* ----------------------- Inclusive Bounded Trigonometric Functions ---------------------------- */

#define CREATE_BOUNDED_TRIGONOMETRIC_CLASS(className, funcName, boundType, lowerBound, upperBound) \
    class Expression##className final                                                              \
        : public ExpressionBoundedTrigonometric<Expression##className, boundType> {                \
    public:                                                                                        \
        explicit Expression##className(const boost::intrusive_ptr<ExpressionContext>& expCtx)      \
            : ExpressionBoundedTrigonometric(expCtx) {}                                            \
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
    };                                                                                             \
    REGISTER_EXPRESSION(funcName, Expression##className::parse);


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


#define CREATE_TRIGONOMETRIC_CLASS(className, funcName)                                       \
    class Expression##className final                                                         \
        : public ExpressionUnboundedTrigonometric<Expression##className> {                    \
    public:                                                                                   \
        explicit Expression##className(const boost::intrusive_ptr<ExpressionContext>& expCtx) \
            : ExpressionUnboundedTrigonometric(expCtx) {}                                     \
                                                                                              \
        double doubleFunc(double arg) const final {                                           \
            return std::funcName(arg);                                                        \
        }                                                                                     \
                                                                                              \
        Decimal128 decimalFunc(Decimal128 arg) const final {                                  \
            return arg.funcName();                                                            \
        }                                                                                     \
                                                                                              \
        const char* getOpName() const final {                                                 \
            return "$" #funcName;                                                             \
        }                                                                                     \
                                                                                              \
        void acceptVisitor(ExpressionVisitor* visitor) final {                                \
            return visitor->visit(this);                                                      \
        }                                                                                     \
    };                                                                                        \
    REGISTER_EXPRESSION(funcName, Expression##className::parse);

CREATE_TRIGONOMETRIC_CLASS(ArcTangent, atan);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicArcSine, asinh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicCosine, cosh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicSine, sinh);
CREATE_TRIGONOMETRIC_CLASS(HyperbolicTangent, tanh);

#undef CREATE_TRIGONOMETRIC_CLASS


/* ----------------------- ExpressionArcTangent2 ---------------------------- */

class ExpressionArcTangent2 final : public ExpressionTwoNumericArgs<ExpressionArcTangent2> {
public:
    explicit ExpressionArcTangent2(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionTwoNumericArgs(expCtx) {}

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
REGISTER_EXPRESSION(atan2, ExpressionArcTangent2::parse);


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
    explicit ExpressionDegreesToRadians(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg(expCtx) {}

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

REGISTER_EXPRESSION(degreesToRadians, ExpressionDegreesToRadians::parse);

class ExpressionRadiansToDegrees final
    : public ExpressionSingleNumericArg<ExpressionRadiansToDegrees> {
public:
    explicit ExpressionRadiansToDegrees(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg(expCtx) {}

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

REGISTER_EXPRESSION(radiansToDegrees, ExpressionRadiansToDegrees::parse);
}  // namespace mongo
