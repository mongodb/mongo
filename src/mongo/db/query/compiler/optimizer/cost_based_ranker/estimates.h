/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fixed_string.h"
#include "mongo/util/modules.h"

#include <chrono>
#include <cmath>
#include <compare>
#include <limits>
#include <string_view>

#include <boost/functional/hash.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::cost_based_ranker {

/**
 * We're using the `nsec(123.45)` constructor to avoid `""ns`,
 * because floating point UDLs are not constexpr on PPC,
 * due to a gcc bug handling `long double`.
 */
using nsec = std::chrono::duration<double, std::nano>;

template <typename Rep, typename Period>
constexpr double toDoubleMillis(std::chrono::duration<Rep, Period> d) {
    return duration_cast<std::chrono::duration<double, std::milli>>(d).count();
}


/**
 * Approximate comparison of double numbers within 'epsilon'.
 * Adapted from: https://floating-point-gui.de/errors/comparison/
 */
bool nearlyEqual(double a, double b, double epsilon);

/**
 * Measurement units of all types of estimates with the following meaning:
 * UnitLess - the measured quantity is a ratio, typically used for selectivity
 * DataItems - the unit of cardinality estimates - applies both to documents and keys
 * CostUnits - the unit of cost estimates measured in abstract cost units
 * CostPerDataItem - the unit of cost coefficients measured in cost per data item
 */
#define ESTIMATION_UNITS_NAMES(F) \
    F(UnitLess)                   \
    F(DataItems)                  \
    F(CostUnits)                  \
    F(CostPerDataItem)
QUERY_UTIL_NAMED_ENUM_DEFINE(EstimationUnit, ESTIMATION_UNITS_NAMES);
#undef ESTIMATION_UNITS_NAMES

/**
 * Sources of cardinality (and consequently cost) estimates. The sources have the following meaning:
 * Unknown - no information about the source - a default that should not appear in estimates
 * Histogram - the estimate is computed via histogram CE
 * Sampling  - the estimate is computed via sampling CE
 * Heuristics  - the estimate is computed via heuristic CE
 * Mixed  - the estimate is a result of some computation that mixes estimates of different types
 * Metadata - the estimate comes from database metadata like collection cardinality
 * Code - the estimate is computed directly in C++, for instance a constant, expression, or
 *         computation via exactCE. Most often used for internal testing.
 */
#define ESTIMATION_SOURCE_NAMES(F) \
    F(Histogram)                   \
    F(Sampling)                    \
    F(Heuristics)                  \
    F(Mixed)                       \
    F(Metadata)                    \
    F(Code)                        \
    F(Unknown)
QUERY_UTIL_NAMED_ENUM_DEFINE(EstimationSource, ESTIMATION_SOURCE_NAMES);
#undef ESTIMATION_SOURCE_NAMES

/**
 * A meta-class that describes a specific instantiation of a strong double type.
 * Each type instance is described by several parameters below. Notice that the parameters
 * are extracted from a parameter-holding type 'paramType'. This approach was implemented to
 * circumvent a limitation of the current clang compiler. In principle the parameters could
 * be provided directly as double values when clang is upgraded to version >=18 which supports
 * double non-type parameters.
 */
template <FixedString nameArg, EstimationUnit unitArg, typename numType, typename paramType>
struct StrongDoubleTag {
    // the name of the property quantified by this type
    static constexpr std::string_view kName = nameArg;
    // the units used to measure this quantity
    static constexpr EstimationUnit kUnit = unitArg;
    // the minimum value of this data type, inclusive
    static constexpr numType kMinValue = paramType::kMin;
    // the maximum value of this data type, inclusive
    static constexpr numType kMaxValue = paramType::kMax;
    // two values are considered equal if their relative error is less than 'epsilon'
    static constexpr numType kEpsilon = paramType::kEpsilon;
};

// Structures that package the non-type template parameters of 'StrongDoubleTag' below.
// They are necessary in order circumvent a limitation of the current clang compiler that doesn't
// accept directly double non-type template parameters.

struct CardinalityTagParam {
    static constexpr double kMin = 0.0;
    static constexpr double kMax = std::numeric_limits<double>::max();
    // For context - this constant results in the following absolute errors:
    // +/-1000 docs in 1M, +/-10 docs in 10K, +/-1 doc in 1K
    static constexpr double kEpsilon = 1.0e-4;
};

struct SelectivityTagParam {
    static constexpr double kMin = 0.0;
    static constexpr double kMax = 1.0;
    // This precision means that for instance selectivities
    // between {0.00980, 0.01020} are considered equal to 0.01.
    static constexpr double kEpsilon = 0.01;
};

struct CostCoefficientTagParam {
    // The smallest cost coefficient is equal to the cost of the fastest QE
    // operation. This is typically the cost of a simple binary comparison of a
    // scalar value.
    static constexpr double kMin = toDoubleMillis(nsec(11.67));
    // The maximum value of a cost coefficient is the most expensive operation per
    // document according to the cost model.
    static constexpr double kMax = toDoubleMillis(nsec(1e6));
    // TODO (SERVER-94981): Define this value based on cost model sensitivity.
    static constexpr double kEpsilon = 1.0e-5;
};

struct CostTagParam {
    static constexpr double kMin = 0.0;
    static constexpr double kMax = std::numeric_limits<double>::max();
    static constexpr double kEpsilon = 1.0e-5;
};

using CardinalityTag = StrongDoubleTag<"Cardinality",
                                       // Cardinality is measured in documents.
                                       EstimationUnit::DataItems,
                                       double,
                                       CardinalityTagParam>;

using SelectivityTag = StrongDoubleTag<"Selectivity",
                                       // Selectivity does not have units, it is a ratio.
                                       EstimationUnit::UnitLess,
                                       double,
                                       SelectivityTagParam>;

using CostTag = StrongDoubleTag<"Cost",
                                // Cost has some abstract cost units.
                                EstimationUnit::CostUnits,
                                double,
                                CostTagParam>;

using CostCoefficientTag =
    StrongDoubleTag<"Cost coefficient",
                    // Cost coefficients establish the cost of processing per one input document.
                    EstimationUnit::CostPerDataItem,
                    double,
                    CostCoefficientTagParam>;

template <class T1, class T2>
class OptimizerEstimate;
class CardinalityEstimate;
class CostEstimate;
class CostCoefficient;
class SelectivityEstimate;

/**
 * Strong double type. Used to represent cardinality, selectivity and cost estimates.
 */
template <class TypeTag>
class StrongDouble {
public:
    explicit constexpr StrongDouble(double value) : _v(value) {
        assertValid();
    }

    constexpr StrongDouble() = delete;

    StrongDouble<TypeTag>& operator=(const StrongDouble<TypeTag>& other) {
        _v = other._v;
        assertValid();
        return *this;
    }

    bool operator==(const StrongDouble<TypeTag>& other) const = default;

    auto operator<=>(const StrongDouble& other) const = default;

    // The minimum and maximum values of this type, inclusive.
    static StrongDouble<TypeTag> minValue() {
        static StrongDouble<TypeTag> theValue(TypeTag::kMinValue);
        return theValue;
    }

    static StrongDouble<TypeTag> maxValue() {
        static StrongDouble<TypeTag> theValue(TypeTag::kMaxValue);
        return theValue;
    }

    static constexpr double epsilon() {
        return TypeTag::kEpsilon;
    }

    // The name of this strong type. It is possible to extract it via typeid, but it is
    // unnecessarily complex. This approach is simpler and more flexible.
    static constexpr std::string_view name() {
        return TypeTag::kName;
    }

    void assertValid() const {
        tassert(9274201,
                str::stream() << "Invalid " << name() << " value < minValue: (" << _v << " < "
                              << minValue()._v << ")",
                _v >= TypeTag::kMinValue);
        tassert(9274200,
                str::stream() << "Invalid " << name() << " value > maxValue: (" << _v << " > "
                              << maxValue()._v << ")",
                _v <= TypeTag::kMaxValue);
    }

    template <typename T1, typename T2>
    friend class OptimizerEstimate;
    friend class SelectivityEstimate;

    friend SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                                         const CardinalityEstimate& bigger_ce);

    friend CardinalityEstimate operator*(const SelectivityEstimate& s,
                                         const CardinalityEstimate& ce);
    friend CardinalityEstimate operator/(const CardinalityEstimate& ce,
                                         const SelectivityEstimate& s);

private:
    // Raw value accessor, private on purpose: estimates expose their value only through
    // OptimizerEstimate::toDouble(), the single intentional escape hatch. Befriended operators and
    // OptimizerEstimate access this (and _v) directly.
    double v() const {
        return _v;
    }

    double _v;
};

using CardinalityType = StrongDouble<CardinalityTag>;
using SelectivityType = StrongDouble<SelectivityTag>;
using CostType = StrongDouble<CostTag>;
using CostCoefficientType = StrongDouble<CostCoefficientTag>;

/**
 * Non-templated base class for OptimizerEstimate containing non-dependent class template members.
 * According to: 'https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines'
 * '#t62-place-non-dependent-class-template-members-in-a-non-templated-base-class'
 */
class EstimateBase {
public:
    EstimateBase() = delete;

    constexpr EstimateBase(EstimationSource s) : _source(s) {};

    /**
     * Merge this estimate with another one - used in operators that combine two estimates.
     * The source of this estimate is adjusted depending on the 'other' estimate.
     * Merging follows the following general rules:
     * - Source pairs are symmetric.
     * - Any type of source merged with itself results in the same type of source.
     * - Merging Code with any other type of source results in the other type.
     * - Merging Metadata with any other type of source except Code results in the other type.
     * - The combination of any other two different types of sources results in the Mixed type.
     *
     * These rules result in the following state table:
     * --------------------------------------------------------------------
     *            | Histogram  Sampling Heuristics Mixed  Metadata   Code
     * --------------------------------------------------------------------
     * Histogram  | Histogram  Mixed    Mixed      Mixed  Histogram  Histogram
     * Sampling   |            Sampling Mixed      Mixed  Sampling   Sampling
     * Heuristics |                     Heuristics Mixed  Heuristics Heuristics
     * Mixed      |                                Mixed  Mixed      Mixed
     * Metadata   |                                       Metadata   Metadata
     * Code       |                                                  Code
     */
    void mergeSources(const EstimateBase& other);

protected:
    EstimationSource _source{EstimationSource::Unknown};
};

/**
 * Generic optimizer estimate that is specialized via the 'EstimateType' parameter which is itself
 * a specilisation of a StrongDouble. This class implements all operations common across all kinds
 * of estimates.
 *
 * In order for the resulting values of the operators be of the specific subclasses that inherit
 * from OptimizerEstimate, the declaration of this class uses the The Curiously Recurring Template
 * Pattern (CRTP) approach where the template parameters are:
 * - 'ValueType' is the type of the estimate that each subclass holds, that is, a specific
 *   StrongDouble.
 * - 'EstimateType' is the subtype of OptimizerEstimate itself.
 */
template <class ValueType, class EstimateType>
class OptimizerEstimate : public EstimateBase {
public:
    OptimizerEstimate() = delete;

    constexpr OptimizerEstimate(ValueType e, EstimationSource s) : EstimateBase(s), _estimate(e) {}

    void assertValid() const {
        _estimate.assertValid();
    }

    // Escape hatch returning the raw underlying value. Use ONLY for transcendental / numeric-model
    // math (pow/log, byte/page computations) and serialization. Do NOT use it for comparison or
    // arithmetic the library already supports - prefer the exact*/approx* comparisons, product(),
    // ratio(), saturatingSubtract(), exactMin/exactMax, etc., so intent stays explicit.
    double toDouble() const {
        return _estimate._v;
    }

    EstimationSource source() const {
        return _source;
    }

    struct Hasher {
        size_t operator()(const OptimizerEstimate& e) const {
            size_t hash = 0;
            boost::hash_combine(hash, e._source);
            boost::hash_combine(hash, e._estimate._v);
            return hash;
        }
    };

    std::string toString() const {
        std::stringstream ss;
        ss << estimateName() << ": " << _estimate._v << ", "
           << "Source: " << sourceName();
        return ss.str();
    }

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append(estimateName(), _estimate._v);
        bob.append("Source", sourceName());
        return bob.obj();
    }

    OptimizerEstimate& operator=(const OptimizerEstimate& other) = default;

    // Comparison operators
    bool operator==(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return this == &e || nearlyEqual(this->_estimate._v, e._estimate._v, ValueType::epsilon());
    }

    bool operator!=(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return !(*this == e);
    }

    // Arithmetic operators.
    // Addition and subtraction are applicable to most kinds of optimizer estimates.
    // They are deleted explicitly wherever they are not applicable.
    // On the other hand, multiplication and division cannot be used to combine arbitrary
    // estimates. These operations are implemented below only the meaningful combinations
    // of estimate subtypes.
    EstimateType& operator+=(const EstimateType& e) {
        this->mergeSources(e);
        double newV = this->_estimate._v + e._estimate._v;
        // Comparison on estimates is approximate while arithmetic is exact, so a sum whose true
        // value is at most the maximum can land just above it due to floating-point rounding.
        // Clamp such epsilon overshoots to the maximum; a larger overflow is a real logic error
        // and still trips the assertion.
        //
        // Note this finite epsilon overshoot is only possible when the maximum is a small value
        // (e.g. SelectivityTag's 1.0). When the maximum is DBL_MAX (CardinalityTag), a sum that
        // exceeds it cannot be epsilon-above the max — it can only round to +inf — and +inf is not
        // nearlyEqual to DBL_MAX by any epsilon, so for that tag a genuine overflow always asserts.
        const double maxV = ValueType::maxValue().v();
        if (MONGO_unlikely(newV > maxV)) {
            tassert(12552501,
                    str::stream() << "Addition of " << ValueType::name() << " overflowed to "
                                  << newV << " from " << this->_estimate._v << " and "
                                  << e._estimate._v,
                    nearlyEqual(newV, maxV, ValueType::epsilon()));
            newV = maxV;
        }
        this->_estimate._v = newV;
        assertValid();
        return *static_cast<EstimateType*>(this);
    }

    EstimateType operator+(const EstimateType& e) const {
        EstimateType result(*static_cast<const EstimateType*>(this));
        result += e;
        return result;
    }

    EstimateType& operator-=(const EstimateType& e) {
        this->mergeSources(e);
        double newV = this->_estimate._v - e._estimate._v;
        // Comparison on estimates is approximate while arithmetic is exact, so subtracting a value
        // that is approximately equal to (but exactly larger than) this estimate can dip just below
        // the minimum. Clamp such epsilon underflows to the minimum; a larger underflow is a
        // real logic error and still trips the assertion.
        //
        // Unlike the addition overshoot above, this underflow clamp applies to every current tag:
        // all minimums are 0.0, and doubles are dense near 0, so an epsilon underflow is a finite,
        // representable value just below 0 (not -inf). The guard tests whether the two operands are
        // nearlyEqual (their difference is then ~0), which is the right check given a 0.0 minimum.
        const double minV = ValueType::minValue().v();
        if (MONGO_unlikely(newV < minV)) {
            tassert(12552500,
                    str::stream() << "Subtraction of " << ValueType::name() << " underflowed to "
                                  << newV << " from " << this->_estimate._v << " and "
                                  << e._estimate._v,
                    nearlyEqual(this->_estimate._v, e._estimate._v, ValueType::epsilon()));
            newV = minV;
        }
        this->_estimate._v = newV;
        assertValid();
        return *static_cast<EstimateType*>(this);
    }

    EstimateType operator-(const EstimateType& e) const {
        EstimateType result(*static_cast<const EstimateType*>(this));
        result -= e;
        return static_cast<EstimateType>(result);
    }

private:
    // Epsilon-approximate three-way comparison. Private on purpose: epsilon-equality is not
    // transitive, so this is not a strict weak ordering and must never back the relational
    // operators or std::min/std::max/std::sort. 'approxCompare' is the sole sanctioned entry point;
    // callers choose an explicit exact*/approx* helper (see the comparison utilities below).
    auto operator<=>(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return *this == e ? std::partial_ordering::equivalent
                          : this->_estimate._v <=> e._estimate._v;
    }

    template <class V, class E>
    friend std::partial_ordering approxCompare(const OptimizerEstimate<V, E>& a,
                                               const OptimizerEstimate<V, E>& b);

protected:
    std::string sourceName() const {
        return std::string{toStringData(_source)};
    }

    std::string estimateName() const {
        return std::string{ValueType::name()};
    }

    ValueType _estimate;
};

/**
 * Model Cardinality estimates.
 */
class CardinalityEstimate : public OptimizerEstimate<CardinalityType, CardinalityEstimate> {
public:
    CardinalityEstimate() = delete;

    constexpr CardinalityEstimate(CardinalityType ce, EstimationSource src)
        : OptimizerEstimate(ce, src) {}

    CardinalityType cardinality() const {
        return _estimate;
    }

    // Convert this cardinality to an integer count by flooring it (which, since cardinalities are
    // non-negative, matches a plain 'size_t(toDouble())' truncation). Prefer this over casting
    // toDouble() so the intent is explicit at the call site.
    size_t toCount() const {
        return static_cast<size_t>(std::floor(toDouble()));
    }

    // Multiplication is undefined for two CEs - this operation has no meaning - the unit of the
    // result would be documents^2. However, it is common to multiply CE by some unitless factor,
    // or multiply a cost coefficient with CE to get the cost of a node's output.
    friend CardinalityEstimate operator*(const CardinalityEstimate& ce, double factor);

    friend CardinalityEstimate operator*(double factor, const CardinalityEstimate& ce);

    friend CostEstimate operator*(const CardinalityEstimate& ce, const CostCoefficient& cc);

    friend CostEstimate operator*(const CostCoefficient& cc, const CardinalityEstimate& ce);

    friend SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                                         const CardinalityEstimate& bigger_ce);

    friend CardinalityEstimate operator*(const SelectivityEstimate& s,
                                         const CardinalityEstimate& ce);

    friend CardinalityEstimate operator*(const CardinalityEstimate& ce,
                                         const SelectivityEstimate& s);

    friend CardinalityEstimate operator/(const CardinalityEstimate& ce,
                                         const SelectivityEstimate& s);
};

/**
 * Model Cost estimates.
 */
class CostEstimate : public OptimizerEstimate<CostType, CostEstimate> {
public:
    CostEstimate() = delete;

    constexpr CostEstimate(CostType c, EstimationSource src) : OptimizerEstimate(c, src) {}

    CostType cost() const {
        return _estimate;
    }

    // Multiplication is undefined for two costs - this operation has no meaning - the unit of the
    // result would be CostEstimate^2. However, it is useful to multiply costs by some unitless
    // factor.
    friend CostEstimate operator*(const CostEstimate& c, double factor);

    friend CostEstimate operator*(double factor, const CostEstimate& c);
};

/**
 * Model Cost coefficients. Their unit is "cost unit / document".
 */
class CostCoefficient : public OptimizerEstimate<CostCoefficientType, CostCoefficient> {
public:
    CostCoefficient() = delete;

    // Cost coefficients are stored in C++ code. In the future we can envision other ways to
    // derive cost coefficients - for instance user-supplied or automatic (re-)calibration.
    constexpr CostCoefficient(CostCoefficientType cc)
        : OptimizerEstimate(cc, EstimationSource::Code) {}

    // Addition and subtraction do not make sense for cost coefficients
    CostCoefficient& operator+=(const CostCoefficient& e) = delete;

    CostCoefficient& operator+(const CostCoefficient& e) const = delete;

    CostCoefficient& operator-=(const CostCoefficient& e) = delete;

    CostCoefficient& operator-(const CostCoefficient& e) const = delete;

    // Cost coefficients can only be multiplied with CE to produce cost.
    friend CostEstimate operator*(const CardinalityEstimate& ce, const CostCoefficient& cc);

    friend CostEstimate operator*(const CostCoefficient& cc, const CardinalityEstimate& ce);
};

template <typename Rep, typename Period>
constexpr CostCoefficient makeCostCoefficient(std::chrono::duration<Rep, Period> d) {
    return CostCoefficient{CostCoefficientType{toDoubleMillis(d)}};
}


class SelectivityEstimate : public OptimizerEstimate<SelectivityType, SelectivityEstimate> {
public:
    SelectivityEstimate() = delete;

    constexpr SelectivityEstimate(SelectivityType sel, EstimationSource src)
        : OptimizerEstimate(sel, src) {}

    SelectivityEstimate operator*(const SelectivityEstimate s) {
        SelectivityEstimate result(*this);
        result.mergeSources(s);
        result._estimate._v *= s._estimate._v;
        assertValid();
        return result;
    }

    SelectivityEstimate pow(double exp) const {
        SelectivityEstimate result(*this);
        result._estimate._v = std::pow(result._estimate._v, exp);
        assertValid();
        return result;
    }

    SelectivityEstimate negate() const {
        SelectivityEstimate result(*this);
        result._estimate._v = 1 - result._estimate._v;
        assertValid();
        return result;
    }

    // Generally selectivity can be produced by (a) an estimate produced by some CE method, or
    // (b) dividing two cardinalities. This function implements (b).
    friend SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                                         const CardinalityEstimate& bigger_ce);

    friend CardinalityEstimate operator*(const SelectivityEstimate& s,
                                         const CardinalityEstimate& ce);

    friend CardinalityEstimate operator*(const CardinalityEstimate& ce,
                                         const SelectivityEstimate& s);

    friend CardinalityEstimate operator/(const CardinalityEstimate& ce,
                                         const SelectivityEstimate& s);
};

CardinalityEstimate operator*(const CardinalityEstimate& ce, double factor);

CardinalityEstimate operator*(double factor, const CardinalityEstimate& ce);

CostEstimate operator*(const CostCoefficient& cc, const CardinalityEstimate& ce);

CostEstimate operator*(const CardinalityEstimate& ce, const CostCoefficient& cc);

SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                              const CardinalityEstimate& bigger_ce);

CardinalityEstimate operator*(const SelectivityEstimate& s, const CardinalityEstimate& ce);

CardinalityEstimate operator*(const CardinalityEstimate& ce, const SelectivityEstimate& s);

/**
 * Product of two cardinalities (count x count -> count). Dimensionally unusual, so it is a named
 * function rather than operator*: use it only where a combinatorial product is genuinely intended
 * (e.g. the rows examined by a nested-loop join, or seeks = NDV x seeks-per-distinct-value).
 */
CardinalityEstimate product(const CardinalityEstimate& a, const CardinalityEstimate& b);

/**
 * Scale a cost by a repetition count (cost x count -> cost), e.g. the total cost of performing an
 * operation 'ce' times.
 */
CostEstimate operator*(const CostEstimate& c, const CardinalityEstimate& ce);
CostEstimate operator*(const CardinalityEstimate& ce, const CostEstimate& c);

/**
 * Dimensionless ratio of two costs (cost / cost -> double). Returns a plain double on purpose: the
 * result is unitless and is not itself an estimate.
 */
double ratio(const CostEstimate& a, const CostEstimate& b);

/**
 * Comparison utilities for estimates.
 *
 * Estimates intentionally do NOT expose the relational operators (<, <=, >, >=) publicly: the
 * underlying operator<=> is epsilon-approximate, and epsilon-equality is not transitive, so it is
 * not a strict weak ordering and must never back std::min/std::max/std::sort (that is undefined
 * behavior). Callers pick an explicit comparison instead:
 *
 *   - exact*  : compares the underlying values bit-exactly. Use when you need a true ordering
 *               (e.g. std::sort with an exactLt/exactGt comparator), a hard bound
 *               (exactMin/exactMax are guaranteed <= / >= BOTH operands), or to reproduce a
 *               former raw-double comparison.
 *   - approx* : epsilon-tolerant, consistent with operator==. Use for pairwise decisions where two
 *               estimates within epsilon should count as tied (e.g. plan-ranking cost comparisons).
 *               PAIRWISE ONLY -- never pass approx* into an ordered algorithm. On an epsilon tie
 *               approxMin/approxMax may return a value up to epsilon larger/smaller than the other
 *               operand; use exactMin/exactMax when a guaranteed bound matters.
 */
template <class ValueType, class EstimateType>
std::partial_ordering exactCompare(const OptimizerEstimate<ValueType, EstimateType>& a,
                                   const OptimizerEstimate<ValueType, EstimateType>& b) {
    return a.toDouble() <=> b.toDouble();
}

template <class ValueType, class EstimateType>
std::partial_ordering approxCompare(const OptimizerEstimate<ValueType, EstimateType>& a,
                                    const OptimizerEstimate<ValueType, EstimateType>& b) {
    return a <=> b;
}

template <class ValueType, class EstimateType>
bool exactLt(const OptimizerEstimate<ValueType, EstimateType>& a,
             const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_lt(exactCompare(a, b));
}

template <class ValueType, class EstimateType>
bool exactLtEq(const OptimizerEstimate<ValueType, EstimateType>& a,
               const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_lteq(exactCompare(a, b));
}

template <class ValueType, class EstimateType>
bool exactGt(const OptimizerEstimate<ValueType, EstimateType>& a,
             const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_gt(exactCompare(a, b));
}

template <class ValueType, class EstimateType>
bool exactGtEq(const OptimizerEstimate<ValueType, EstimateType>& a,
               const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_gteq(exactCompare(a, b));
}

template <class ValueType, class EstimateType>
bool approxLt(const OptimizerEstimate<ValueType, EstimateType>& a,
              const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_lt(approxCompare(a, b));
}

template <class ValueType, class EstimateType>
bool approxLtEq(const OptimizerEstimate<ValueType, EstimateType>& a,
                const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_lteq(approxCompare(a, b));
}

template <class ValueType, class EstimateType>
bool approxGt(const OptimizerEstimate<ValueType, EstimateType>& a,
              const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_gt(approxCompare(a, b));
}

template <class ValueType, class EstimateType>
bool approxGtEq(const OptimizerEstimate<ValueType, EstimateType>& a,
                const OptimizerEstimate<ValueType, EstimateType>& b) {
    return std::is_gteq(approxCompare(a, b));
}

template <class ValueType, class EstimateType>
EstimateType exactMin(const OptimizerEstimate<ValueType, EstimateType>& a,
                      const OptimizerEstimate<ValueType, EstimateType>& b) {
    return static_cast<const EstimateType&>(a.toDouble() <= b.toDouble() ? a : b);
}

template <class ValueType, class EstimateType>
EstimateType exactMax(const OptimizerEstimate<ValueType, EstimateType>& a,
                      const OptimizerEstimate<ValueType, EstimateType>& b) {
    return static_cast<const EstimateType&>(a.toDouble() >= b.toDouble() ? a : b);
}

// Pairwise epsilon-tolerant min/max. On an epsilon tie these return 'b' (mirroring the
// '(a < b) ? a : b' ternaries they replace). Per the note above, the result is not a guaranteed
// bound -- use exactMin/exactMax when that matters.
template <class ValueType, class EstimateType>
EstimateType approxMin(const OptimizerEstimate<ValueType, EstimateType>& a,
                       const OptimizerEstimate<ValueType, EstimateType>& b) {
    return static_cast<const EstimateType&>(approxLt(a, b) ? a : b);
}

template <class ValueType, class EstimateType>
EstimateType approxMax(const OptimizerEstimate<ValueType, EstimateType>& a,
                       const OptimizerEstimate<ValueType, EstimateType>& b) {
    return static_cast<const EstimateType&>(approxGt(a, b) ? a : b);
}

/**
 * Saturating ("truncated") subtraction: max(0, a - b), using exact comparison. For domains where
 * the subtrahend legitimately exceeding the minuend means zero (e.g. a $skip past the end of its
 * input), so it never asserts -- unlike operator-, which treats a non-epsilon underflow as a bug.
 */
CardinalityEstimate saturatingSubtract(const CardinalityEstimate& a, const CardinalityEstimate& b);

/**
 * The actual strategy used to generate the sample.
 */
enum class SamplingTechnique {
    kRandom,
    kChunk,
    kFullCollScan,
    kSeqScan,
    kStrides,
};

/**
 * Metadata about the sample used when 'ceSource == Sampling'.
 */
struct SamplingMetadata {
    bool isPersisted;
    size_t docCount;           // number of documents in the sample
    size_t requestedDocCount;  // number of documents originally requested
    size_t memorySizeBytes;
    SamplingTechnique technique;
    boost::optional<int> numChunks;
    boost::optional<Date_t> createdAt;
};

/**
 * The optimizer's estimate of a single QSN in the physical plan.
 */
struct QSNEstimate {
    // A QSN may have three estimates:
    // - the number of processed data items (docs or keys): 'inCE'
    // - the number of produced data items: 'outCE'
    // - the number of index seeks: 'indexSeekCE'
    // Only leaf QSN nodes have both 'inCE' and 'outCE' estimates. All other nodes have an
    // 'out' CE since their input size is equal to the number of produced items by their child.
    // For instance:
    // - For a CollectionScan with a filter expression, 'inCE' is the total collection cardinality,
    //   and 'outCE' is the number of documents after applying the filter.
    // - For an IndexScan node 'inCE' is the number of scanned keys, 'outCE' is the number of
    //   keys after applying a possible filter expression to the matching keys.
    // Only IndexScanNodes will have a corresponding indexSeekCE
    boost::optional<CardinalityEstimate> inCE;
    CardinalityEstimate outCE{CardinalityType{0}, EstimationSource::Code};
    boost::optional<CardinalityEstimate> indexSeekCE;
    // Sentinel: default-initialized to the maximum representable cost so that an un-estimated plan
    // ranks as the worst possible candidate. The cost estimator always overwrites this with a real
    // estimate before a plan is ranked. Any further optimization/estimation is supposed to improve
    // on the default maximum cost.
    CostEstimate cost{CostType::maxValue(), EstimationSource::Code};

    QSNEstimate() = default;
    QSNEstimate(CardinalityEstimate outCE,
                CostEstimate cost = CostEstimate{CostType::maxValue(), EstimationSource::Code})
        : outCE(std::move(outCE)), cost(std::move(cost)) {}
    QSNEstimate(boost::optional<CardinalityEstimate> inCE, CardinalityEstimate outCE)
        : inCE(std::move(inCE)), outCE(std::move(outCE)) {}

    virtual ~QSNEstimate() = default;

    virtual void serialize(BSONObjBuilder& bob) const {
        bob.append("costEstimate", cost.toDouble());
        bob.append("cardinalityEstimate", outCE.toDouble());
        BSONObjBuilder metadataBob(bob.subobjStart("estimatesMetadata"));
        metadataBob.append("ceSource", toStringData(outCE.source()));
        metadataBob.done();
    }
};

// Predefined constants
inline const CardinalityEstimate zeroCE{CardinalityType{0.0}, EstimationSource::Code};
inline const CardinalityEstimate zeroMetadataCE{CardinalityType{0.0}, EstimationSource::Metadata};
inline const CardinalityEstimate oneCE{CardinalityType{1}, EstimationSource::Code};
inline const CardinalityEstimate minCE{CardinalityType::minValue(), EstimationSource::Code};
inline const CardinalityEstimate maxCE{CardinalityType::maxValue(), EstimationSource::Code};

// TODO(SERVER-100603): Remove these hardcoded values once we can estimate them
inline constexpr int32_t kAverageDocumentSizeBytes = 1024;
inline constexpr int32_t kAverageIndexEntrySizeBytes = 256;

inline const SelectivityEstimate zeroSel{SelectivityType{0.0}, EstimationSource::Code};
inline const SelectivityEstimate oneSel{SelectivityType{1.0}, EstimationSource::Code};

inline const CostCoefficient minCC{CostCoefficientType::minValue()};

inline const CostEstimate zeroCost{CostType{0.0}, EstimationSource::Code};
// No query plan can be cheaper than running the cheapest operation for a single input value.
static const CostEstimate minCost{CostType{CostCoefficientTag::kMinValue}, EstimationSource::Code};
inline const CostEstimate maxCost{CostType::maxValue(), EstimationSource::Code};

}  // namespace mongo::cost_based_ranker
