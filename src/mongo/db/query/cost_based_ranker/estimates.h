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

#include <boost/functional/hash.hpp>
#include <limits>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fixed_string.h"

namespace mongo::cost_based_ranker {

constexpr double nsToMs = 1.0e-6;

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
 * Code - the estimate is computed directly in C++, for instance a constant, or expression.
 *         Most often used for internal testing.
 */
#define ESTIMATION_SOURCE_NAMES(F) \
    F(Unknown)                     \
    F(Histogram)                   \
    F(Sampling)                    \
    F(Heuristics)                  \
    F(Mixed)                       \
    F(Metadata)                    \
    F(Code)
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
    static constexpr StringData kName = nameArg;
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
    static constexpr double kMin = 0.0;
    static constexpr double kMax = std::numeric_limits<double>::max();
    static constexpr double kEpsilon = 1.0e-5;
};

struct CostTagParam {
    // The smallest cost coefficient is equal to the cost of the fastest QE
    // operation. This is typically the cost of a simple binary comparison of a
    // scalar value.
    // TODO (SERVER-94981): based on Bonsai cost calibration it is 1 ns, assuming
    //  all cost calibration measurements are in 'ms'. Should be updated with a
    //  reference to the relevant cost coefficient in the new cost model.
    static constexpr double kMin = 50 * nsToMs;
    // The maximum value of a cost coefficient is the most expensive operation per
    // document according to the cost model.
    // TODO (SERVER-94981): Currently this is based on Bonsai calibration, and it
    //  should be updated to reference the relevant cost coefficient in the new cost
    //  model.
    static constexpr double kMax = 15000 * nsToMs;
    // TODO (SERVER-94981): Define this value based on cost model sensitivity.
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
                                CostCoefficientTagParam>;

using CostCoefficientTag =
    StrongDoubleTag<"Cost coefficient",
                    // Cost coefficients establish the cost of processing per one input document.
                    EstimationUnit::CostPerDataItem,
                    double,
                    CostTagParam>;

template <typename T>
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
    static constexpr StringData name() {
        return TypeTag::kName;
    }

    double v() const {
        return _v;
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

    template <typename T>
    friend class OptimizerEstimate;
    friend class SelectivityEstimate;
    friend SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                                         const CardinalityEstimate& bigger_ce);

    friend CardinalityEstimate operator*(const SelectivityEstimate& s,
                                         const CardinalityEstimate& ce);

private:
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
    EstimateBase(EstimationSource s) : _source(s){};

    /**
     * Merge this estimate with another one - used in operators that combine two estimates.
     * The source of this estimate is adjusted depending on the 'other' estimate.
     */
    void mergeSources(const EstimateBase& other);

protected:
    EstimationSource _source{EstimationSource::Unknown};
};

/**
 * Generic optimizer estimate that is specialized via the 'EstimateType' parameter which is itself
 * a specilisation of a StrongDouble. This class implements all operations common across all kinds
 * of estimates.
 */
template <class EstimateType>
class OptimizerEstimate : public EstimateBase {
public:
    OptimizerEstimate() = delete;

    OptimizerEstimate(EstimateType e, EstimationSource s) : EstimateBase(s), _estimate(e) {}

    void assertValid() const {
        _estimate.assertValid();
    }

    // Cast the estimate to double
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
    bool operator==(const OptimizerEstimate<EstimateType>& e) const {
        return this == &e ||
            nearlyEqual(this->_estimate._v, e._estimate._v, EstimateType::epsilon());
    }

    bool operator!=(const OptimizerEstimate<EstimateType>& e) const {
        return !(*this == e);
    }

    bool operator>(const OptimizerEstimate<EstimateType>& e) const {
        return (*this != e) && this->_estimate._v > e._estimate._v;
    }

    bool operator>=(const OptimizerEstimate<EstimateType>& e) const {
        return (*this == e) || this->_estimate._v > e._estimate._v;
    }

    bool operator<(const OptimizerEstimate<EstimateType>& e) const {
        return (*this != e) && this->_estimate._v < e._estimate._v;
    }

    bool operator<=(const OptimizerEstimate<EstimateType>& e) const {
        return (*this == e) || this->_estimate._v < e._estimate._v;
    }

    // Arithmetic operators.
    // Addition and subtraction are applicable to most kinds of optimizer estimates.
    // They are deleted explicitly wherever they are not applicable.
    // On the other hand, multiplication and division cannot be used to combine arbitrary
    // estimates. These operations are implemented below only the meaningful combinations
    // of estimate subtypes.
    OptimizerEstimate<EstimateType>& operator+=(const OptimizerEstimate<EstimateType>& e) {
        this->mergeSources(e);
        this->_estimate._v += e._estimate._v;
        assertValid();
        return *this;
    }
    OptimizerEstimate<EstimateType> operator+(const OptimizerEstimate<EstimateType>& e) const {
        OptimizerEstimate<EstimateType> result(*this);
        result += e;
        return result;
    }

    OptimizerEstimate<EstimateType>& operator-=(const OptimizerEstimate<EstimateType>& e) {
        this->mergeSources(e);
        this->_estimate._v -= e._estimate._v;
        assertValid();
        return *this;
    }

    OptimizerEstimate<EstimateType> operator-(const OptimizerEstimate<EstimateType>& e) const {
        OptimizerEstimate<EstimateType> result(*this);
        result -= e;
        return result;
    }

protected:
    std::string sourceName() const {
        return std::string{toStringData(_source)};
    }

    std::string estimateName() const {
        return std::string{EstimateType::name()};
    }

    EstimateType _estimate;
};

/**
 * Model Cardinality estimates.
 */
class CardinalityEstimate : public OptimizerEstimate<CardinalityType> {
public:
    CardinalityEstimate() = delete;

    CardinalityEstimate(CardinalityType ce, EstimationSource src) : OptimizerEstimate(ce, src) {}

    CardinalityType cardinality() const {
        return _estimate;
    }

    // Multiplication is undefined for two CEs - this operation has no meaning - the unit of the
    // result would be documents^2. However, it is common to multiply CE by some unitless factor,
    // or multiply cost with CE to get the cost of a node's output.
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
};

/**
 * Model Cost estimates.
 */
class CostEstimate : public OptimizerEstimate<CostType> {
public:
    CostEstimate() = delete;

    CostEstimate(CostType c, EstimationSource src) : OptimizerEstimate(c, src) {}

    CostType cost() const {
        return _estimate;
    }
};

/**
 * Model Cost coefficients. Their unit is "cost unit / document".
 */
class CostCoefficient : public OptimizerEstimate<CostCoefficientType> {
public:
    CostCoefficient() = delete;

    // Cost coefficients are stored in C++ code. In the future we can envision other ways to
    // derive cost coefficients - for instance user-supplied or automatic (re-)calibration.
    CostCoefficient(CostCoefficientType cc) : OptimizerEstimate(cc, EstimationSource::Code) {}

    // Addition and subtraction do not make sense for cost coefficients
    CostCoefficient& operator+=(const CostCoefficient& e) = delete;
    CostCoefficient& operator+(const CostCoefficient& e) const = delete;
    CostCoefficient& operator-=(const CostCoefficient& e) = delete;
    CostCoefficient& operator-(const CostCoefficient& e) const = delete;

    // Cost coefficients can only be multiplied with CE to produce cost.
    friend CostEstimate operator*(const CardinalityEstimate& ce, const CostCoefficient& cc);
    friend CostEstimate operator*(const CostCoefficient& cc, const CardinalityEstimate& ce);
};


class SelectivityEstimate : public OptimizerEstimate<SelectivityType> {
public:
    SelectivityEstimate() = delete;

    SelectivityEstimate(SelectivityType sel, EstimationSource src) : OptimizerEstimate(sel, src) {}

    SelectivityEstimate operator*(const SelectivityEstimate s) {
        SelectivityEstimate result(*this);
        result._estimate._v *= s._estimate._v;
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
};

CardinalityEstimate operator*(const CardinalityEstimate& ce, double factor);
CardinalityEstimate operator*(double factor, const CardinalityEstimate& ce);
CostEstimate operator*(const CostCoefficient& cc, const CardinalityEstimate& ce);
CostEstimate operator*(const CardinalityEstimate& ce, const CostCoefficient& cc);
SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                              const CardinalityEstimate& bigger_ce);
CardinalityEstimate operator*(const SelectivityEstimate& s, const CardinalityEstimate& ce);
CardinalityEstimate operator*(const CardinalityEstimate& ce, const SelectivityEstimate& s);

// Predefined constants
inline const CardinalityEstimate zeroCE(CardinalityType{0.0}, EstimationSource::Code);
inline const CardinalityEstimate minCE(CardinalityType::minValue(), EstimationSource::Code);
inline const CardinalityEstimate maxCE(CardinalityType::maxValue(), EstimationSource::Code);

}  // namespace mongo::cost_based_ranker
