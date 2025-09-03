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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/ce_utils.h"

namespace mongo::cost_based_ranker {

/**
 * Comparator for optimizer estimate types which uses the underlying double for comparison instead
 * of using the epsilon based comparison. This is useful for sorting containers of estimates using
 * the STL, which requires that comparators establish an equivalence relationship (if a == b and b
 * == c, then a == c). The epsilon based implementation of operator== breaks this assumption.
 * See https://en.cppreference.com/w/cpp/named_req/Compare.
 */
template <typename T, bool less = true>
struct ExactEstimateComparator {
    bool operator()(const T& lhs, const T& rhs) const {
        if constexpr (less) {
            return lhs.toDouble() < rhs.toDouble();
        }
        return lhs.toDouble() > rhs.toDouble();
    }
};

/**
 * Conditionally negate selectivity.
 */
template <bool negate>
constexpr SelectivityEstimate maybeNegate(const SelectivityEstimate s) {
    if constexpr (negate) {
        return s.negate();
    }
    return s;
}

/**
 * Computes conjunctive and disjunctive exponential backoff. We first take the extreme selectivities
 * (the smallest for conjunction, or the largest for disjunction). We then multiply them together
 * (inverting them for disjunction), and then for disjunction we invert the result, applying
 * increasing decay factor for each larger/smaller selectivity.
 */
template <bool isConjunction,
          class Comparator =
              typename std::conditional_t<isConjunction,
                                          ExactEstimateComparator<SelectivityEstimate>,
                                          ExactEstimateComparator<SelectivityEstimate, false>>>
SelectivityEstimate expBackoffInternal(std::span<SelectivityEstimate> sels) {
    if (sels.size() == 1) {
        return sels[0];
    }

    const size_t actualMaxBackoffElements = std::min(sels.size(), kMaxBackoffElements);
    std::partial_sort(
        sels.begin(), sels.begin() + actualMaxBackoffElements, sels.end(), Comparator());

    SelectivityEstimate sel{SelectivityType{1.0}, EstimationSource::Code};
    double f = 1.0;
    for (size_t i = 0; i < actualMaxBackoffElements; i++, f /= 2.0) {
        // TODO: implement operator*=
        sel = sel * maybeNegate<!isConjunction>(sels[i]).pow(f);
    }
    return maybeNegate<!isConjunction>(sel);
}

SelectivityEstimate conjExponentialBackoff(std::span<SelectivityEstimate> conjSelectivities) {
    tassert(9582601,
            "The array of conjunction selectivities may not be empty.",
            !conjSelectivities.empty());
    return expBackoffInternal<true /*isConjunction*/>(conjSelectivities);
}

SelectivityEstimate disjExponentialBackoff(std::span<SelectivityEstimate> disjSelectivities) {
    tassert(9582602,
            "The array of disjunction selectivities may not be empty.",
            !disjSelectivities.empty());
    return expBackoffInternal<false /*isConjunction*/>(disjSelectivities);
}

void addFieldsToRelevantIndexOutput(const BSONObj& keyPattern, StringSet& relevantIndexOutput) {
    const auto& keyNames = keyPattern.getFieldNames<StringSet>();
    for (const auto& keyName : keyNames) {
        auto dotPos = keyName.find('.');
        relevantIndexOutput.insert(
            keyName.substr(0, dotPos != std::string::npos ? dotPos : keyName.size()));
    }
}

}  // namespace mongo::cost_based_ranker
