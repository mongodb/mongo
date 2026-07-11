// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

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

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    const char* getOpName() const final {
        return "$atan2";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionArcTangent2>(&expCtx, cloneChildren(expCtx));
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
        Value evaluate(const Document& root,                                                       \
                       Variables* variables,                                                       \
                       const EvaluationContext& ctx) const final;                                  \
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
        boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {            \
            return make_intrusive<Expression##className>(&expCtx, cloneChildren(expCtx));          \
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


#define CREATE_TRIGONOMETRIC_CLASS(className, funcName)                                   \
    class Expression##className final                                                     \
        : public ExpressionUnboundedTrigonometric<Expression##className> {                \
    public:                                                                               \
        explicit Expression##className(ExpressionContext* const expCtx)                   \
            : ExpressionUnboundedTrigonometric(expCtx) {}                                 \
        explicit Expression##className(ExpressionContext* const expCtx,                   \
                                       ExpressionVector&& children)                       \
            : ExpressionUnboundedTrigonometric(expCtx, std::move(children)) {}            \
                                                                                          \
        Value evaluate(const Document& root,                                              \
                       Variables* variables,                                              \
                       const EvaluationContext& ctx) const final;                         \
                                                                                          \
        const char* getOpName() const final {                                             \
            return "$" #funcName;                                                         \
        }                                                                                 \
                                                                                          \
        void acceptVisitor(ExpressionMutableVisitor* visitor) final {                     \
            return visitor->visit(this);                                                  \
        }                                                                                 \
                                                                                          \
        void acceptVisitor(ExpressionConstVisitor* visitor) const final {                 \
            return visitor->visit(this);                                                  \
        }                                                                                 \
        boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {   \
            return make_intrusive<Expression##className>(&expCtx, cloneChildren(expCtx)); \
        }                                                                                 \
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

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    const char* getOpName() const final {
        return "$degreesToRadians";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }


    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionDegreesToRadians>(&expCtx, cloneChildren(expCtx));
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

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    const char* getOpName() const final {
        return "$radiansToDegrees";
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionRadiansToDegrees>(&expCtx, cloneChildren(expCtx));
    }
};


}  // namespace mongo
