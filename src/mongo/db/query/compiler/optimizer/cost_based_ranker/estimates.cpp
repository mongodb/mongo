// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"

#include <algorithm>

namespace mongo::cost_based_ranker {

bool nearlyEqual(double a, double b, double epsilon) {
    if (a == b) {
        return true;  // shortcut, handles infinities
    }

    double absA = std::abs(a);
    double absB = std::abs(b);
    double diff = std::abs(a - b);

    if (a == 0 || b == 0 || (absA + absB < std::numeric_limits<double>::min())) {
        // a or b is zero or both are extremely close to it
        // relative error is less meaningful in this case
        return diff < (epsilon * std::numeric_limits<double>::min());
    } else {
        // use relative error
        return diff / std::min((absA + absB), std::numeric_limits<double>::max()) < epsilon;
    }
}

/**
 * OptimizerEstimate methods and operators
 */

void EstimateBase::mergeSources(const EstimateBase& other) {
    EstimationSource s1 = this->_source;
    EstimationSource s2 = other._source;

    // It is illegal to perform operations on estimates with unknown source
    tassert(9274204, "This estimate has unknown source", s1 != EstimationSource::Unknown);
    tassert(9274203, "Other estimate has unknown source", s2 != EstimationSource::Unknown);

    if (s1 == s2) {
        return;  // This covers the diagonal of the state matrix in the header comment
    }

    // Define the rules of source merging as a state transition table.
    auto getSource = [](EstimationSource a, EstimationSource b) -> EstimationSource {
        if (a > b) {
            std::swap(a, b);  // Ensure a <= b to handle symmetric cases
        }
        switch (a) {
            case EstimationSource::Histogram:
                switch (b) {
                    case EstimationSource::Sampling:
                    case EstimationSource::Heuristics:
                    case EstimationSource::Mixed:
                        return EstimationSource::Mixed;
                    case EstimationSource::Metadata:
                    case EstimationSource::Code:
                        return EstimationSource::Histogram;
                    default:
                        MONGO_UNREACHABLE_TASSERT(9695207);
                }
            case EstimationSource::Sampling:
                switch (b) {
                    case EstimationSource::Mixed:
                    case EstimationSource::Heuristics:
                        return EstimationSource::Mixed;
                        return EstimationSource::Mixed;
                    case EstimationSource::Metadata:
                    case EstimationSource::Code:
                        return EstimationSource::Sampling;
                    default:
                        MONGO_UNREACHABLE_TASSERT(9695206);
                }
            case EstimationSource::Heuristics:
                switch (b) {
                    case EstimationSource::Mixed:
                        return EstimationSource::Mixed;
                    case EstimationSource::Metadata:
                    case EstimationSource::Code:
                        return EstimationSource::Heuristics;
                    default:
                        MONGO_UNREACHABLE_TASSERT(9695205);
                }
            case EstimationSource::Mixed:
                switch (b) {
                    case EstimationSource::Metadata:
                    case EstimationSource::Code:
                        return EstimationSource::Mixed;
                    default:
                        MONGO_UNREACHABLE_TASSERT(9695204);
                }
            case EstimationSource::Metadata:
                switch (b) {
                    case EstimationSource::Code:
                        return EstimationSource::Metadata;
                    default:
                        MONGO_UNREACHABLE_TASSERT(9695203);
                }
            default:
                MONGO_UNREACHABLE_TASSERT(9695201);
        }
        MONGO_UNREACHABLE_TASSERT(9695200);
    };

    _source = getSource(s1, s2);
}

/**
 * Functions / operators with arguments of different types
 */

CardinalityEstimate operator*(const CardinalityEstimate& ce, double factor) {
    return {CardinalityType{ce.toDouble() * factor}, ce.source()};
}

CardinalityEstimate operator*(double factor, const CardinalityEstimate& ce) {
    return ce * factor;
}

CostEstimate operator*(const CostCoefficient& cc, const CardinalityEstimate& ce) {
    return {CostType{cc.toDouble() * ce.toDouble()}, ce.source()};
}
CostEstimate operator*(const CardinalityEstimate& ce, const CostCoefficient& cc) {
    return cc * ce;
}

CostEstimate operator*(const CostEstimate& c, double factor) {
    return {CostType{c.toDouble() * factor}, c.source()};
}

CostEstimate operator*(double factor, const CostEstimate& c) {
    return c * factor;
}

SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                              const CardinalityEstimate& bigger_ce) {
    // Prevent undefined selectivity.
    tassert(9967301,
            str::stream{} << "selectivity undefined with 0 cardinality denominator",
            bigger_ce._estimate._v > 0.0);

    SelectivityEstimate result(SelectivityType{0.0}, smaller_ce._source);
    result.mergeSources(bigger_ce);

    // Reconcile the approximate comparison used by callers with the exact arithmetic done here.
    // When the operands are approximately equal the selectivity is 1.0; checking this first absorbs
    // a numerator that is within epsilon larger than the denominator, which
    // would otherwise trip the strict precondition below.
    if (smaller_ce == bigger_ce) {
        result._estimate._v = 1.0;
        return result;
    }

    // A numerator genuinely (more than epsilon) larger than the denominator is a logic error.
    tassert(9274202,
            str::stream() << smaller_ce._estimate.v() << " must be < " << bigger_ce._estimate.v()
                          << " to produce selectivity",
            smaller_ce._estimate.v() < bigger_ce._estimate.v());

    result._estimate._v = std::clamp(smaller_ce._estimate.v() / bigger_ce._estimate.v(), 0.0, 1.0);
    return result;
}

CardinalityEstimate operator*(const SelectivityEstimate& s, const CardinalityEstimate& ce) {
    CardinalityEstimate result(ce);
    result.mergeSources(s);
    result._estimate._v *= s._estimate.v();
    result.assertValid();
    return result;
}

CardinalityEstimate operator*(const CardinalityEstimate& ce, const SelectivityEstimate& s) {
    return s * ce;
}

CardinalityEstimate operator/(const CardinalityEstimate& ce, const SelectivityEstimate& s) {
    CardinalityEstimate result(ce);
    result.mergeSources(s);
    result._estimate._v /= s._estimate.v();
    result.assertValid();
    return result;
}

CardinalityEstimate product(const CardinalityEstimate& a, const CardinalityEstimate& b) {
    // Constructing CardinalityType validates the result.
    CardinalityEstimate result{CardinalityType{a.toDouble() * b.toDouble()}, a.source()};
    result.mergeSources(b);
    return result;
}

CostEstimate operator*(const CostEstimate& c, const CardinalityEstimate& ce) {
    CostEstimate result{CostType{c.toDouble() * ce.toDouble()}, c.source()};
    result.mergeSources(ce);
    return result;
}

CostEstimate operator*(const CardinalityEstimate& ce, const CostEstimate& c) {
    return c * ce;
}

double ratio(const CostEstimate& a, const CostEstimate& b) {
    return a.toDouble() / b.toDouble();
}

CardinalityEstimate saturatingSubtract(const CardinalityEstimate& a, const CardinalityEstimate& b) {
    // Exact comparison: only subtract when a is strictly larger, otherwise the result is zero.
    // This deliberately never asserts - callers use it where b >= a is a legitimate outcome.
    if (exactGt(a, b)) {
        return a - b;
    }
    CardinalityEstimate result{CardinalityType{0.0}, a.source()};
    result.mergeSources(b);
    return result;
}

}  // namespace mongo::cost_based_ranker
