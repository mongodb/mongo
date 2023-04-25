/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <algorithm>   // std::sort
#include <cmath>       // std::pow
#include <functional>  // std::greater

#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer::ce {
bool validSelectivity(SelectivityType sel) {
    return (sel >= 0.0 && sel <= 1.0);
}

bool validCardinality(CEType card) {
    return (card >= kMinCard && card <= std::numeric_limits<double>::max());
}

/**
 * Conditionally negate selectivity.
 */
template <bool negate>
constexpr SelectivityType maybeNegate(const SelectivityType s) {
    return negateSel(s);
}
template <>
constexpr SelectivityType maybeNegate<false>(const SelectivityType s) {
    return s;
}

/**
 * Computes conjunctive and disjunctive exponential backoff. We first take the extreme selectivities
 * (the smallest for conjunction, or the largest for disjunction). We then multiply them together
 * (inverting them for disjunction), and then for disjunction we invert the result, applying
 * increasing decay factor for each larger/smaller selectivity.
 */
template <bool isConjunction,
          class Comparator = typename std::conditional_t<isConjunction,
                                                         std::less<SelectivityType>,
                                                         std::greater<SelectivityType>>>
SelectivityType expBackoffInternal(std::vector<SelectivityType> sels) {
    if (sels.size() == 1) {
        return sels[0];
    }

    const size_t actualMaxBackoffElements = std::min(sels.size(), kMaxBackoffElements);
    std::partial_sort(
        sels.begin(), sels.begin() + actualMaxBackoffElements, sels.end(), Comparator());

    SelectivityType sel{1.0};
    double f = 1.0;
    for (size_t i = 0; i < actualMaxBackoffElements; i++, f /= 2.0) {
        sel *= maybeNegate<!isConjunction>(sels[i]).pow(f);
    }

    return maybeNegate<!isConjunction>(sel);
}

SelectivityType conjExponentialBackoff(std::vector<SelectivityType> conjSelectivities) {
    uassert(6749501,
            "The array of conjunction selectivities may not be empty.",
            !conjSelectivities.empty());
    return expBackoffInternal<true /*isConjunction*/>(std::move(conjSelectivities));
}

SelectivityType disjExponentialBackoff(std::vector<SelectivityType> disjSelectivities) {
    uassert(6749502,
            "The array of disjunction selectivities may not be empty.",
            !disjSelectivities.empty());
    return expBackoffInternal<false /*isConjunction*/>(std::move(disjSelectivities));
}
}  // namespace mongo::optimizer::ce
