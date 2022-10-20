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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <map>
#include <ostream>
#include <queue>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/max_diff.h"
#include "mongo/db/query/ce/value_utils.h"
#include "mongo/util/assert_util.h"


namespace mongo::ce {

std::string printDistribution(const DataDistribution& distr, size_t nElems) {
    std::ostringstream os;
    for (size_t i = 0; i < std::min(nElems, distr._freq.size()); ++i) {
        os << "{val: " << distr._bounds[i].get() << ", " << distr._freq[i].toString() << "}\n";
    }
    return os.str();
}

static double valueSpread(value::TypeTags tag1,
                          value::Value val1,
                          value::TypeTags tag2,
                          value::Value val2) {
    double doubleVal1 = valueToDouble(tag1, val1);
    double doubleVal2 = valueToDouble(tag2, val2);
    uassert(6660502,
            "Data distribution values must be monotonically increasing.",
            doubleVal2 >= doubleVal1);
    return doubleVal2 - doubleVal1;
}

DataDistribution getDataDistribution(const std::vector<SBEValue>& sortedInput) {
    if (sortedInput.empty()) {
        return {};
    }

    DataDistribution result;
    value::TypeTags prevTag;
    value::Value prevValue;
    bool first = true;

    // Aggregate the values in a sorted dataset into a frequency distribution.
    size_t idx = 0;
    for (size_t i = 0; i < sortedInput.size(); i++) {
        const auto v = sortedInput[i].get();
        const auto comparison = first ? 1 : compareValues(v.first, v.second, prevTag, prevValue);
        first = false;

        if (comparison != 0) {
            uassert(6660550, "Input is not sorted", comparison > 0);
            prevTag = v.first;
            prevValue = v.second;

            const auto [tagCopy, valCopy] = copyValue(v.first, v.second);
            result._bounds.emplace_back(tagCopy, valCopy);
            result._freq.emplace_back(idx, 1);
            ++idx;
        } else {
            ++result._freq.back()._freq;
        }
    }

    // Calculate the area for all values in the data distribution.
    // The current minimum and maximum areas of the values of a type class.
    double maxArea = 0.0;

    for (size_t i = 0; i + 1 < result._freq.size(); ++i) {
        const auto v1 = result._bounds[i];
        const auto v2 = result._bounds[i + 1];
        const bool newTypeClass = !sameTypeClass(v1.getTag(), v2.getTag());

        if (newTypeClass) {
            const auto res = result.typeClassBounds.emplace(i, maxArea);
            uassert(6660551, "There can't be duplicate type class bounds.", res.second);
            maxArea = 0.0;
        } else if (i == 0) {
            const double spread =
                valueSpread(v1.getTag(), v1.getValue(), v2.getTag(), v2.getValue());
            maxArea = result._freq[i]._freq * spread;
        }

        if (i == 0 || newTypeClass) {
            // Make sure we insert bucket boundaries between different types, and also make sure
            // first value is picked for a boundary.
            result._freq[i]._area = std::numeric_limits<double>::infinity();
        } else {
            const double spread =
                valueSpread(v1.getTag(), v1.getValue(), v2.getTag(), v2.getValue());
            result._freq[i]._area = result._freq[i]._freq * spread;
            maxArea = std::max(maxArea, result._freq[i]._area);
        }
    }

    // Make sure last value is picked as a histogram bucket boundary.
    result._freq.back()._area = std::numeric_limits<double>::infinity();
    const auto res = result.typeClassBounds.emplace(result._freq.size(), maxArea);
    uassert(6660503, "There can't be duplicate type class bounds.", res.second);

    // Compute normalized areas. If the spread is 0, the area may also be 0. This could happen,
    // for instance, if there is only a single value of a given type,
    size_t beginIdx = 0;
    for (const auto [endIdx, area] : result.typeClassBounds) {
        for (size_t i = beginIdx; i < endIdx; ++i) {
            result._freq[i]._normArea = area > 0.0 ? (result._freq[i]._area / area) : 0.0;
        }
        beginIdx = endIdx;
    }

    // std::cout << "Distribution sorted by value:\n"
    //           << printDistribution(result, result._freq.size()) << "\n"
    //           << std::flush;

    return result;
}

// TODO: This doesn't seem right -- it looks like we're sorting on the frequency,
//       not the difference between buckets
static std::vector<ValFreq> generateTopKBuckets(const DataDistribution& dataDistrib,
                                                size_t numBuckets) {
    struct AreaComparator {
        bool operator()(const ValFreq& a, const ValFreq& b) const {
            return a._normArea > b._normArea;
        }
    };
    std::priority_queue<ValFreq, std::vector<ValFreq>, AreaComparator> pq;

    for (const auto& valFreq : dataDistrib._freq) {
        if (pq.size() < numBuckets) {
            pq.emplace(valFreq);
        } else if (AreaComparator()(valFreq, pq.top())) {
            pq.pop();
            pq.emplace(valFreq);
        }
    }

    std::vector<ValFreq> result;
    while (!pq.empty()) {
        result.push_back(pq.top());
        pq.pop();
    }

    std::sort(result.begin(), result.end(), [](const ValFreq& a, const ValFreq& b) {
        return a._idx < b._idx;
    });

    return result;
}

ScalarHistogram genMaxDiffHistogram(const DataDistribution& dataDistrib, size_t numBuckets) {
    if (dataDistrib._freq.empty()) {
        return {};
    }

    std::vector<ValFreq> topKBuckets = generateTopKBuckets(dataDistrib, numBuckets);
    uassert(6660504,
            "Must have bucket boundary on first value",
            topKBuckets[0]._idx == dataDistrib._freq[0]._idx);
    uassert(6660505,
            "Must have bucket boundary on last value",
            topKBuckets.back()._idx == dataDistrib._freq.back()._idx);

    std::vector<Bucket> buckets;
    value::Array bounds;

    // Create histogram buckets out of the top-K bucket values.
    size_t startBucketIdx = 0;
    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;
    for (size_t i = 0; i < std::min(dataDistrib._freq.size(), numBuckets); i++) {
        const size_t bucketBoundIdx = topKBuckets[i]._idx;
        const double freq = dataDistrib._freq.at(bucketBoundIdx)._freq;

        // Compute per-bucket statistics.
        double rangeFreq = 0.0;
        double ndv = 0.0;
        while (startBucketIdx < bucketBoundIdx) {
            rangeFreq += dataDistrib._freq[startBucketIdx++]._freq;
            ++ndv;
        }
        cumulativeFreq += rangeFreq + freq;
        cumulativeNDV += ndv + 1.0;

        // Add a histogram bucket.
        const auto v = dataDistrib._bounds[startBucketIdx];
        const auto [copyTag, copyVal] = value::copyValue(v.getTag(), v.getValue());
        bounds.push_back(copyTag, copyVal);
        buckets.emplace_back(freq, rangeFreq, cumulativeFreq, ndv, cumulativeNDV);
        startBucketIdx++;
    }

    return {std::move(bounds), std::move(buckets)};
}

ArrayHistogram createArrayEstimator(const std::vector<SBEValue>& arrayData, size_t nBuckets) {
    std::vector<SBEValue> scalarData;
    std::vector<SBEValue> arrayMinData;
    std::vector<SBEValue> arrayMaxData;
    std::vector<SBEValue> arrayUniqueData;
    std::map<value::TypeTags, size_t> typeCounts;
    std::map<value::TypeTags, size_t> arrayTypeCounts;
    size_t emptyArrayCount = 0;

    for (const auto& v : arrayData) {
        auto tagCount = typeCounts.insert({v.getTag(), 1});
        if (!tagCount.second) {
            ++tagCount.first->second;
        }
        if (v.getTag() == value::TypeTags::Null) {
            continue;  // nulls are accounted for in typeCounts
        }
        if (v.getTag() == value::TypeTags::Array) {
            std::vector<SBEValue> arrayElements;
            value::Array* arr = value::getArrayView(v.getValue());
            size_t arrSize = arr->size();
            if (arrSize == 0) {
                ++emptyArrayCount;
                continue;
            }

            for (size_t i = 0; i < arrSize; i++) {
                const auto [tag, val] = arr->getAt(i);
                auto arrTagCount = arrayTypeCounts.insert({tag, 1});
                if (!arrTagCount.second) {
                    ++arrTagCount.first->second;
                }
                if (tag == value::TypeTags::Null) {
                    continue;  // nulls are accounted for in arrayTypeCounts
                }
                const auto [tagCopy, valCopy] = value::copyValue(tag, val);
                arrayElements.emplace_back(tagCopy, valCopy);
            }

            sortValueVector(arrayElements);

            {
                // Emit values for arrayMin and arrayMax histograms.
                boost::optional<SBEValue> prev;
                for (const auto& element : arrayElements) {
                    uassert(6660506,
                            "Multi-level array nesting is not supported yet",
                            element.getTag() != value::TypeTags::Array);
                    if (!prev) {
                        arrayMinData.push_back(element);
                    } else if (!sameTypeClass(prev->getTag(), element.getTag())) {
                        arrayMaxData.push_back(*prev);
                        arrayMinData.push_back(element);
                    }
                    prev = element;
                }
                if (prev) {
                    arrayMaxData.push_back(*prev);
                }
            }
            {
                // Emit values for arrayUnique histogram.
                boost::optional<SBEValue> prev;
                for (const auto& element : arrayElements) {
                    if (!prev ||
                        compareValues(prev->getTag(),
                                      prev->getValue(),
                                      element.getTag(),
                                      element.getValue()) < 0) {
                        arrayUniqueData.push_back(element);
                        prev = element;
                    }
                }
            }
        } else {
            // Assume non-arrays are scalars. Emit values for scalar histogram.
            scalarData.push_back(v);
        }
    }

    sortValueVector(scalarData);
    sortValueVector(arrayMinData);
    sortValueVector(arrayMaxData);
    sortValueVector(arrayUniqueData);

    ScalarHistogram scalarHist = genMaxDiffHistogram(getDataDistribution(scalarData), nBuckets);
    ScalarHistogram arrayMinHist = genMaxDiffHistogram(getDataDistribution(arrayMinData), nBuckets);
    ScalarHistogram arrayMaxHist = genMaxDiffHistogram(getDataDistribution(arrayMaxData), nBuckets);
    ScalarHistogram arrayUniqueHist =
        genMaxDiffHistogram(getDataDistribution(arrayUniqueData), nBuckets);

    return {std::move(scalarHist),
            std::move(typeCounts),
            std::move(arrayUniqueHist),
            std::move(arrayMinHist),
            std::move(arrayMaxHist),
            std::move(arrayTypeCounts),
            emptyArrayCount};
}

}  // namespace mongo::ce
