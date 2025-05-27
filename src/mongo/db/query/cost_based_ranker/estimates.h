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

#include <limits>

#include <boost/functional/hash.hpp>

namespace mongo::cost_based_ranker {

/**
 * Convert nanoseconds to milliseconds.
 */
constexpr double operator""_ms(long double v) {
    return v * 1.0e-6;
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
 * Code - the estimate is computed directly in C++, for instance a constant, or expression.
 *         Most often used for internal testing.
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
    // The smallest cost coefficient is equal to the cost of the fastest QE
    // operation. This is typically the cost of a simple binary comparison of a
    // scalar value.
    // TODO (SERVER-94981): based on Bonsai cost calibration it is 1 ns, assuming
    //  all cost calibration measurements are in 'ms'. Should be updated with a
    //  reference to the relevant cost coefficient in the new cost model.
    static constexpr double kMin = 10.0_ms;
    // The maximum value of a cost coefficient is the most expensive operation per
    // document according to the cost model.
    // TODO (SERVER-94981): Currently this is based on Bonsai calibration, and it
    //  should be updated to reference the relevant cost coefficient in the new cost
    //  model.
    static constexpr double kMax = 15000.0_ms;
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

    template <typename T1, typename T2>
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
    bool operator==(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return this == &e || nearlyEqual(this->_estimate._v, e._estimate._v, ValueType::epsilon());
    }

    bool operator!=(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return !(*this == e);
    }

    bool operator>(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return (*this != e) && this->_estimate._v > e._estimate._v;
    }

    bool operator>=(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return (*this == e) || this->_estimate._v > e._estimate._v;
    }

    bool operator<(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return (*this != e) && this->_estimate._v < e._estimate._v;
    }

    bool operator<=(const OptimizerEstimate<ValueType, EstimateType>& e) const {
        return (*this == e) || this->_estimate._v < e._estimate._v;
    }

    // Arithmetic operators.
    // Addition and subtraction are applicable to most kinds of optimizer estimates.
    // They are deleted explicitly wherever they are not applicable.
    // On the other hand, multiplication and division cannot be used to combine arbitrary
    // estimates. These operations are implemented below only the meaningful combinations
    // of estimate subtypes.
    EstimateType& operator+=(const EstimateType& e) {
        this->mergeSources(e);
        this->_estimate._v += e._estimate._v;
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
        this->_estimate._v -= e._estimate._v;
        assertValid();
        return *static_cast<EstimateType*>(this);
    }

    EstimateType operator-(const EstimateType& e) const {
        EstimateType result(*static_cast<const EstimateType*>(this));
        result -= e;
        return static_cast<EstimateType>(result);
    }

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
 * The optimizer's estimate of a single QSN in the physical plan.
 */
struct QSNEstimate {
    // A QSN may have three estimates:
    // - the number of processed data items (docs or keys): 'inCE'
    // - the number of produced data items: 'outCE'
    // Only leaf QSN nodes have both 'inCE' and 'outCE' estimates. All other nodes have an
    // 'out' CE since their input size is equal to the number of produced items by their child.
    // For instance:
    // - For a CollectionScan with a filter expression, 'inCE' is the total collection cardinality,
    //   and 'outCE' is the number of documents after applying the filter.
    // - For an IndexScan node 'inCE' is the number of scanned keys, 'outCE' is the number of
    //   keys after applying a possible filter expression to the matching keys.
    boost::optional<CardinalityEstimate> inCE;
    CardinalityEstimate outCE{CardinalityType{0}, EstimationSource::Code};
    CostEstimate cost{CostType::maxValue(), EstimationSource::Code};
};

// Predefined constants
inline const CardinalityEstimate zeroCE{CardinalityType{0.0}, EstimationSource::Code};
inline const CardinalityEstimate zeroMetadataCE{CardinalityType{0.0}, EstimationSource::Metadata};
inline const CardinalityEstimate oneCE{CardinalityType{1}, EstimationSource::Code};
inline const CardinalityEstimate minCE{CardinalityType::minValue(), EstimationSource::Code};
inline const CardinalityEstimate maxCE{CardinalityType::maxValue(), EstimationSource::Code};

inline const SelectivityEstimate zeroSel{SelectivityType{0.0}, EstimationSource::Code};
inline const SelectivityEstimate oneSel{SelectivityType{1.0}, EstimationSource::Code};

inline const CostCoefficient minCC{CostCoefficientType::minValue()};

inline const CostEstimate zeroCost{CostType{0.0}, EstimationSource::Code};
// No query plan can be cheaper than running the cheapest operation for a single input value.
static const CostEstimate minCost{CostType{CostCoefficientType::minValue().v()},
                                  EstimationSource::Code};
inline const CostEstimate maxCost{CostType::maxValue(), EstimationSource::Code};

}  // namespace mongo::cost_based_ranker
