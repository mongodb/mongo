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

#include "mongo/db/query/cost_based_ranker/estimates.h"

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
    // It is illegal to perform operations on estimates with unknown source
    tassert(
        9274204, "This estimate has unknown source", this->_source != EstimationSource::Unknown);
    tassert(
        9274203, "Other estimate has unknown source", other._source != EstimationSource::Unknown);

    // Merging with a code based estimate does not modify the estimate type
    if (other._source == EstimationSource::Code) {
        return;
    } else if (_source == EstimationSource::Code) {
        _source = other._source;
        return;
    }
    // Merging with a metadata based estimate does not modify the estimate type
    if (other._source == EstimationSource::Metadata) {
        return;
    } else if (_source == EstimationSource::Metadata) {
        _source = other._source;
        return;
    }
    // Different types result in Mixed, otherwise keep it unchanged.
    _source = (_source == other._source) ? _source : EstimationSource::Mixed;
}

/**
 * Functions / operators with arguments of different types
 */

CardinalityEstimate operator*(const CardinalityEstimate& ce, double factor) {
    return {CardinalityType{ce._estimate.v() * factor}, ce._source};
}

CardinalityEstimate operator*(double factor, const CardinalityEstimate& ce) {
    return ce * factor;
}

CostEstimate operator*(const CostCoefficient& cc, const CardinalityEstimate& ce) {
    return {CostType{cc._estimate.v() * ce._estimate.v()}, ce._source};
}
CostEstimate operator*(const CardinalityEstimate& ce, const CostCoefficient& cc) {
    return cc * ce;
}

SelectivityEstimate operator/(const CardinalityEstimate& smaller_ce,
                              const CardinalityEstimate& bigger_ce) {
    // Make sure the underlying double values are in correct relationship to produce selectivity.
    // Using operator<= could still pass when smaller_ce is slightly bigger than bigger_ce.
    tassert(9274202,
            str::stream() << smaller_ce._estimate.v() << " must be <= " << bigger_ce._estimate.v()
                          << " to produce selectivity",
            smaller_ce._estimate.v() <= bigger_ce._estimate.v());

    SelectivityEstimate result(SelectivityType{0.0}, smaller_ce._source);
    result.mergeSources(bigger_ce);
    result._estimate._v =
        (smaller_ce == bigger_ce) ? 1.0 : smaller_ce._estimate.v() / bigger_ce._estimate.v();
    result.assertValid();
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

}  // namespace mongo::cost_based_ranker
