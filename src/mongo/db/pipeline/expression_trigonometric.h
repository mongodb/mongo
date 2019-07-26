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
    explicit ExpressionBoundedTrigonometric(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<BoundedTrigType>(expCtx) {}

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
                              << ", value must in " << BoundType::leftBracket() << getLowerBound()
                              << "," << getUpperBound() << BoundType::rightBracket(),
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
    explicit ExpressionUnboundedTrigonometric(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionSingleNumericArg<TrigType>(expCtx) {}

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
}  // namespace mongo
