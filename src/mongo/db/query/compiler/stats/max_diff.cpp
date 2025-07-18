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

#include "mongo/db/query/compiler/stats/max_diff.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo::stats {
namespace {
namespace value = sbe::value;

std::string printDistribution(const DataDistribution& distr, size_t nElems) {
    std::ostringstream os;
    for (size_t i = 0; i < std::min(nElems, distr._freq.size()); ++i) {
        os << "{val: " << distr._bounds[i].get() << ", " << distr._freq[i].toString() << "}\n";
    }
    return os.str();
}

double valueSpread(value::TypeTags tag1,
                   value::Value val1,
                   value::TypeTags tag2,
                   value::Value val2) {
    double doubleVal1 = valueToDouble(tag1, val1);
    double doubleVal2 = valueToDouble(tag2, val2);
    uassert(6660502,
            fmt::format("Data distribution values must be monotonically increasing, however "
                        "encountered {} before {}",
                        doubleVal1,
                        doubleVal2),
            doubleVal2 >= doubleVal1);
    return doubleVal2 - doubleVal1;
}

/**
 * Helper for getting the input for constructing an array histogram for an array estimator using the
 * values in an array. For each value in `arrayElements`, update the min, max, and unique value
 * vectors. These will be used to generate the corresponding histograms for array values.
 */
void updateMinMaxUniqArrayVals(std::vector<SBEValue>& arrayElements,
                               std::vector<SBEValue>& arrayMinData,
                               std::vector<SBEValue>& arrayMaxData,
                               std::vector<SBEValue>& arrayUniqueData) {

    if (arrayElements.size() == 0) {
        return;
    }

    sortValueVector(arrayElements);

    // Emit values for arrayMin and arrayMax histograms.
    {
        boost::optional<SBEValue> prev;
        for (const auto& element : arrayElements) {
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

    // Emit values for arrayUnique histogram.
    {
        boost::optional<SBEValue> prev;
        for (const auto& element : arrayElements) {
            if (!prev ||
                compareValues(
                    prev->getTag(), prev->getValue(), element.getTag(), element.getValue()) < 0) {
                arrayUniqueData.push_back(element);
                prev = element;
            }
        }
    }
}

// Helper which calculates the the area between two neighboring values, given the frequency of the
// former value. This function truncates areas which are infinite to the largest possible double as
// we use infinity as a sentinel value to denote the transition between type brackets. This is
// because we want to prioritize splitting type brackets into separate buckets over splitting values
// in the same type bracket that are infinitely far apart, so we avoid returning infinity here.
double boundedCalculateArea(SBEValue v1, SBEValue v2, size_t freq) {
    const double spread = valueSpread(v1.getTag(), v1.getValue(), v2.getTag(), v2.getValue());
    uassert(7299702,
            str::stream() << "the value spread between "
                          << std::make_pair(v1.getTag(), v1.getValue()) << " and "
                          << std::make_pair(v2.getTag(), v2.getValue()) << " is NaN",
            !std::isnan(spread));
    const double area = spread * freq;
    if (std::isinf(area)) {
        return std::numeric_limits<double>::max();
    }
    return area;
}

/*
 * Helper for constructing a priority queue with values either compared based on calculated area or
 * area difference.
 */
template <typename P>
std::vector<ValFreq> fillAndReturnPriorityQueueResult(const DataDistribution& dataDistrib,
                                                      size_t numBuckets,
                                                      P& pq) {
    // Populate priority queue with values in data distribution using a specific comparator for a
    // given sorting algorithm.
    for (const auto& valFreq : dataDistrib._freq) {
        if (pq.size() < numBuckets) {
            pq.emplace(valFreq);
        } else if (typename P::value_compare{}(valFreq, pq.top())) {
            pq.pop();
            pq.emplace(valFreq);
        }
    }

    // Transfer values from priority queue into a vector.
    std::vector<ValFreq> result;
    while (!pq.empty()) {
        result.push_back(pq.top());
        pq.pop();
    }

    // Sort the values by value _idx.
    std::sort(result.begin(), result.end(), [](const ValFreq& a, const ValFreq& b) {
        return a._idx < b._idx;
    });

    return result;
}

struct AreaComparator {
    bool operator()(const ValFreq& a, const ValFreq& b) const {
        // TODO: the alternatives below allow to experiment with original vs normalized areas
        // return a._area         > b._area;
        return a._normArea > b._normArea;
    }
};

struct AreaDiffComparator {
    bool operator()(const ValFreq& a, const ValFreq& b) const {
        // TODO: the alternatives below allow to experiment with original vs normalized areas
        // return a._areaDiff > b._areaDiff;
        return a._normAreaDiff > b._normAreaDiff;
    }
};

struct FreqDiffComparator {
    bool operator()(const ValFreq& a, const ValFreq& b) const {
        return (a._freqDiff > b._freqDiff) || (a._freqDiff == b._freqDiff && a._idx > b._idx);
    }
};

inline bool isValidValFreq(std::vector<ValFreq>& valFreqVec) {
    return std::any_of(valFreqVec.begin(), valFreqVec.end(), [](ValFreq valFreq) {
        return valFreq._area >= 0.0 && valFreq._areaDiff >= 0.0 && valFreq._normArea >= 0.0 &&
            valFreq._normAreaDiff >= 0.0;
    });
}

}  // namespace

std::vector<ValFreq> generateTopKBuckets(const DataDistribution& dataDistrib,
                                         size_t numBuckets,
                                         SortArg sortArg) {
    // find the top "numBucket" number of values from the data distribution according to the
    // provided sorting algorithm.
    if (sortArg == SortArg::kAreaDiff) {
        std::priority_queue<ValFreq, std::vector<ValFreq>, AreaDiffComparator> pq;
        // Call the helper function with pq and the given comparator (determined based on the
        // sorting algorithm) to find "numBucket" number of largest normalized area difference
        // values
        return fillAndReturnPriorityQueueResult(dataDistrib, numBuckets, pq);
    } else if (sortArg == SortArg::kArea) {
        std::priority_queue<ValFreq, std::vector<ValFreq>, AreaComparator> pq;
        return fillAndReturnPriorityQueueResult(dataDistrib, numBuckets, pq);
    } else if (sortArg == SortArg::kFreqDiff) {
        std::priority_queue<ValFreq, std::vector<ValFreq>, FreqDiffComparator> pq;
        return fillAndReturnPriorityQueueResult(dataDistrib, numBuckets, pq);
    }
    MONGO_UNREACHABLE_TASSERT(8674814);
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
    constexpr double kInvalidArea = -1.0;
    double maxArea = kInvalidArea;
    double maxAreaDiff = kInvalidArea;

    for (size_t i = 0; i + 1 < result._freq.size(); ++i) {
        const auto v1 = result._bounds[i];
        const auto v2 = result._bounds[i + 1];
        const bool newTypeBracket = !sameTypeBracket(v1.getTag(), v2.getTag());

        if (newTypeBracket) {
            // If maxArea is 0.0, this is because this value is the only value of its type bracket.
            // Because we want to force it to be a bucket, set maxArea to inifinte.
            std::pair<double, double> valPair;
            valPair.first = {maxArea == 0.0 ? kDoubleInf : maxArea};
            valPair.second = {maxAreaDiff == 0.0 ? kDoubleInf : maxAreaDiff};
            const auto res = result.typeClassBounds.emplace(i, valPair);
            uassert(6660551, "There can't be duplicate type class bounds.", res.second);
            maxArea = 0.0;
            maxAreaDiff = 0.0;
        } else if (i == 0) {
            maxArea = boundedCalculateArea(v1, v2, result._freq[i]._freq);
            maxAreaDiff = 0.0;
        }

        if (i == 0 || newTypeBracket) {
            // Make sure we insert bucket boundaries between different types, and also make sure
            // first value is picked for a boundary.
            result._freq[i]._area = kDoubleInf;
            result._freq[i]._areaDiff = kDoubleInf;
            result._freq[i]._freqDiff = kDoubleInf;
        } else {
            result._freq[i]._area = boundedCalculateArea(v1, v2, result._freq[i]._freq);
            maxArea = std::max(maxArea, result._freq[i]._area);
            result._freq[i]._areaDiff = (i == 1 || result._freq[i - 1]._area == kDoubleInf)
                ? result._freq[i]._area
                : std::abs(result._freq[i]._area - result._freq[i - 1]._area);
            result._freq[i]._freqDiff = (i == 1 || result._freq[i - 1]._freqDiff == kDoubleInf)
                ? result._freq[i]._freq
                : ((result._freq[i]._freq > result._freq[i - 1]._freq)
                       ? result._freq[i]._freq - result._freq[i - 1]._freq
                       : result._freq[i - 1]._freq - result._freq[i]._freq);
            maxAreaDiff = std::max(maxAreaDiff, result._freq[i]._areaDiff);
        }
    }

    // Check that maxArea and maxAreaDiff have valid values.
    uassert(8674811,
            "maxArea cannot be a negative value.",
            (result._freq.size() <= 1 || maxArea >= 0.0));
    uassert(8674812,
            "maxAreaDiff cannot be a negative value.",
            (result._freq.size() <= 1 || maxAreaDiff >= 0.0));

    // Make sure last value is picked as a histogram bucket boundary.
    result._freq.back()._area = kDoubleInf;
    result._freq.back()._areaDiff = kDoubleInf;

    std::pair<double, double> valPair;
    valPair.first = {maxArea == 0.0 ? kDoubleInf : maxArea};
    valPair.second = {maxAreaDiff == 0.0 ? kDoubleInf : maxAreaDiff};
    const auto res = result.typeClassBounds.emplace(result._freq.size(), valPair);

    uassert(8674800, "There can't be duplicate type class bounds.", res.second);

    // Compute normalized areas.
    size_t beginIdx = 0;
    for (const auto& [endIdx, areaInfo] : result.typeClassBounds) {
        tassert(7299703,
                str::stream() << "maximum area for type bracket is zero",
                areaInfo.first != 0.0);
        for (size_t i = beginIdx; i < endIdx; ++i) {
            if (std::isinf(result._freq[i]._area)) {
                result._freq[i]._normArea = kDoubleInf;
            } else {
                result._freq[i]._normArea = result._freq[i]._area / areaInfo.first;
            }
            if (std::isinf(result._freq[i]._areaDiff)) {
                result._freq[i]._normAreaDiff = kDoubleInf;
            } else {
                result._freq[i]._normAreaDiff = result._freq[i]._areaDiff / areaInfo.second;
            }
        }
        beginIdx = endIdx;  // Normalize values of the next type.
    }

    uassert(8674813, "ValFreq contains invalid values", isValidValFreq(result._freq));

    return result;
}

ScalarHistogram genMaxDiffHistogram(const DataDistribution& dataDistrib,
                                    size_t numBuckets,
                                    SortArg sortArg) {
    if (dataDistrib._freq.empty()) {
        return ScalarHistogram::make();
    }

    const auto numTypes = dataDistrib.typeClassBounds.size();
    // At the very least, we must have one bucket for the first value in the distribution and one
    // bucket for every type class (except when the first type bracket has a single value, in which
    // case we the number of buckets can equal the number of types).
    // For example, {0, 1, 2, "foo", "bar"} needs buckets with values: 0, 2, and "bar".
    uassert(
        7299701,
        fmt::format("number of buckets: {}, the number of buckets must exceed the number of types "
                    "({}) in the data by 1. For this case the minimum number of buckets is: {}",
                    numBuckets,
                    numTypes,
                    numTypes + 1),
        numBuckets >= (numTypes + 1));

    std::vector<ValFreq> topKBuckets = generateTopKBuckets(dataDistrib, numBuckets, sortArg);
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

    return ScalarHistogram::make(std::move(bounds), std::move(buckets));
}

std::shared_ptr<const CEHistogram> createCEHistogram(const std::vector<SBEValue>& arrayData,
                                                     size_t nBuckets,
                                                     SortArg sortArg) {
    uassert(7120500, "A histogram must have at least one bucket.", nBuckets > 0);

    // Values that will be used as inputs to histogram generation code.
    std::vector<SBEValue> scalarData;
    std::vector<SBEValue> arrayMinData;
    std::vector<SBEValue> arrayMaxData;
    std::vector<SBEValue> arrayUniqueData;

    // Type counters.
    TypeCounts typeCounts;
    TypeCounts arrayTypeCounts;

    // Value counters.
    double emptyArrayCount = 0;
    double trueCount = 0;
    double falseCount = 0;
    double nanCount = 0;

    // Tracks whether we should use the scalar constructor.
    bool isScalar = true;

    for (const auto& v : arrayData) {
        const auto val = v.getValue();
        const auto tag = v.getTag();

        // Increment type counters.
        auto tagCount = typeCounts.insert({tag, 1});
        if (!tagCount.second) {
            ++tagCount.first->second;
        }

        if (tag == value::TypeTags::Array) {
            isScalar = false;

            // If we have an array, we can construct min, max, and unique histograms from its
            // elements, provided that they are histogrammable.
            std::vector<SBEValue> arrayElements;

            value::Array* arr = value::getArrayView(val);
            size_t arrSize = arr->size();
            if (arrSize == 0) {
                ++emptyArrayCount;
                continue;
            }

            // We only count types once per occurrence per array for histogram CE.
            std::set<value::TypeTags> perArrayTags;
            for (size_t i = 0; i < arrSize; i++) {
                const auto [elemTag, elemVal] = arr->getAt(i);

                perArrayTags.insert(elemTag);
                if (!canEstimateTypeViaHistogram(elemTag)) {
                    // If the elements of this array are not histogrammable, then we can only update
                    // the array type counters; we cannot add this value to the histogram.
                    continue;
                }
                double doubleVal = valueToDouble(elemTag, elemVal);
                if (std::isnan(doubleVal)) {
                    if (!sbe::value::isNumber(elemTag)) {
                        uasserted(7280701,
                                  str::stream() << "Non numeric tag type"
                                                << " is interpreted as NaN");
                    }
                    // Do not add NaNs to array elements.
                    continue;
                }

                const auto [tagCopy, valCopy] = value::copyValue(elemTag, elemVal);
                arrayElements.emplace_back(tagCopy, valCopy);
            }

            // Increment array type tag counts.
            for (auto elemTag : perArrayTags) {
                auto arrTagCount = arrayTypeCounts.insert({elemTag, 1});
                if (!arrTagCount.second) {
                    ++arrTagCount.first->second;
                }
            }

            updateMinMaxUniqArrayVals(arrayElements, arrayMinData, arrayMaxData, arrayUniqueData);

        } else if (tag == value::TypeTags::Boolean) {
            // If we have a boolean, we also have counters for true and false values we should
            // increment here.
            if (value::bitcastTo<bool>(val)) {
                trueCount++;
            } else {
                falseCount++;
            }
            continue;
        } else if (!canEstimateTypeViaHistogram(tag)) {
            // If we have a non-histogrammable type, we can only increment the type counters for it;
            // we cannot build a scalar histogram on it.
            continue;

        } else if (double dVal = valueToDouble(tag, val); std::isnan(dVal)) {
            if (!sbe::value::isNumber(tag)) {
                uasserted(7280702,
                          str::stream()
                              << "Non numeric tag type " << tag << " is interpreted as NaN");
            }
            // We count NaNs separately, check for NaN here.
            nanCount++;
        } else {
            // Assume non-arrays are scalars. Emit values for the scalar histogram.
            scalarData.push_back(v);
        }
    }

    // Lambda helper to construct histogram from an unsorted value vector.
    const auto makeHistogram = [&nBuckets, sortArg](std::vector<SBEValue>& values) {
        sortValueVector(values);
        return genMaxDiffHistogram(getDataDistribution(values), nBuckets, sortArg);
    };

    if (isScalar) {
        // If we don't have array elements, we don't include array fields in the final histogram.
        return CEHistogram::make(makeHistogram(scalarData),
                                 std::move(typeCounts),
                                 arrayData.size(),
                                 trueCount,
                                 falseCount,
                                 nanCount);
    }

    return CEHistogram::make(makeHistogram(scalarData),
                             std::move(typeCounts),
                             makeHistogram(arrayUniqueData),
                             makeHistogram(arrayMinData),
                             makeHistogram(arrayMaxData),
                             std::move(arrayTypeCounts),
                             arrayData.size(),
                             emptyArrayCount,
                             trueCount,
                             falseCount,
                             nanCount);
}

}  // namespace mongo::stats
