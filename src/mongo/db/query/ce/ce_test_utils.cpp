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

#include "mongo/db/query/ce/ce_test_utils.h"

#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/query/stats/rand_utils_new.h"
#include "mongo/db/query/stats/value_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::ce {

size_t calculateFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                          stats::SBEValue valueToCalculate,
                                          bool includeScalar) {
    int actualCard = 0;
    for (const auto& value : data) {
        if (value.getTag() == TypeTags::Array) {
            auto array = sbe::value::getArrayView(value.getValue());

            bool matched = std::any_of(
                array->values().begin(), array->values().end(), [&](const auto& element) {
                    return mongo::stats::compareValues(element.first,
                                                       element.second,
                                                       valueToCalculate.getTag(),
                                                       valueToCalculate.getValue()) == 0;
                });

            if (matched) {
                actualCard++;
            }
        } else {
            if (includeScalar) {
                if (mongo::stats::compareValues(value.getTag(),
                                                value.getValue(),
                                                valueToCalculate.getTag(),
                                                valueToCalculate.getValue()) == 0) {
                    actualCard++;
                }
            }
        }
    }
    return actualCard;
}

size_t calculateTypeFrequencyFromDataVectorEq(const std::vector<stats::SBEValue>& data,
                                              sbe::value::TypeTags type) {
    int actualCard = 0;
    for (const auto& value : data) {
        if (type == value.getTag()) {
            actualCard++;
        }
    }
    return actualCard;
}

size_t calculateFrequencyFromDataVectorRange(const std::vector<stats::SBEValue>& data,
                                             stats::SBEValue valueToCalculateLow,
                                             stats::SBEValue valueToCalculateHigh) {
    int actualCard = 0;
    for (const auto& value : data) {
        // Higher OR equal to low AND lower OR equal to high.
        if (((mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateLow.getTag(),
                                          valueToCalculateLow.getValue()) > 0) ||
             (mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateLow.getTag(),
                                          valueToCalculateLow.getValue()) == 0)) &&
            ((mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateHigh.getTag(),
                                          valueToCalculateHigh.getValue()) < 0) ||
             (mongo::stats::compareValues(value.getTag(),
                                          value.getValue(),
                                          valueToCalculateHigh.getTag(),
                                          valueToCalculateHigh.getValue()) == 0))) {
            actualCard++;
        }
    }
    return actualCard;
}

void populateTypeDistrVectorAccordingToInputConfig(stats::TypeDistrVector& td,
                                                   const std::pair<size_t, size_t>& interval,
                                                   const TypeCombination& typeCombination,
                                                   const size_t ndv,
                                                   std::mt19937_64& seedArray,
                                                   stats::MixedDistributionDescriptor& mdd,
                                                   int arrayLength) {
    for (auto type : typeCombination) {

        switch (type.typeTag) {
            case sbe::value::TypeTags::Nothing:
            case sbe::value::TypeTags::Null:
                td.push_back(
                    std::make_unique<stats::NullDistribution>(mdd, type.typeProbability, ndv));
                break;
            case sbe::value::TypeTags::Boolean: {
                bool includeFalse = false, includeTrue = false;
                if (!(bool)interval.first || !(bool)interval.second) {
                    includeFalse = true;
                }
                if ((bool)interval.first || (bool)interval.second) {
                    includeTrue = true;
                }
                td.push_back(std::make_unique<stats::BooleanDistribution>(mdd,
                                                                          type.typeProbability,
                                                                          (int)includeFalse +
                                                                              (int)includeTrue,
                                                                          includeFalse,
                                                                          includeTrue));
                break;
            }
            case sbe::value::TypeTags::NumberInt32:
            case sbe::value::TypeTags::NumberInt64:
                td.push_back(std::make_unique<stats::IntDistribution>(mdd,
                                                                      type.typeProbability,
                                                                      ndv,
                                                                      interval.first,
                                                                      interval.second,
                                                                      0 /*nullsRatio*/,
                                                                      type.nanProb));
                break;
            case sbe::value::TypeTags::NumberDouble:
                td.push_back(std::make_unique<stats::DoubleDistribution>(mdd,
                                                                         type.typeProbability,
                                                                         ndv,
                                                                         interval.first,
                                                                         interval.second,
                                                                         0 /*nullsRatio*/,
                                                                         type.nanProb));
                break;
            case sbe::value::TypeTags::StringSmall:
            case sbe::value::TypeTags::StringBig:
                td.push_back(std::make_unique<stats::StrDistribution>(
                    mdd, type.typeProbability, ndv, interval.first, interval.second));
                break;
            case sbe::value::TypeTags::Array: {
                stats::TypeDistrVector arrayData;
                arrayData.push_back(std::make_unique<stats::IntDistribution>(
                    mdd, type.typeProbability, ndv, interval.first, interval.second));
                auto arrayDataDesc =
                    std::make_unique<stats::DatasetDescriptorNew>(std::move(arrayData), seedArray);
                td.push_back(std::make_unique<stats::ArrDistribution>(mdd,
                                                                      1.0 /*weight*/,
                                                                      10 /*ndv*/,
                                                                      0 /*minArraLen*/,
                                                                      arrayLength /*maxArrLen*/,
                                                                      std::move(arrayDataDesc)));
                break;
            }
            default:
                MONGO_UNREACHABLE;
                break;
        }
    }
}

void generateDataUniform(size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         const size_t seed,
                         const size_t ndv,
                         std::vector<stats::SBEValue>& data,
                         int arrayLength) {
    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor uniform{{stats::DistrType::kUniform, 1.0}};
    stats::TypeDistrVector td;

    populateTypeDistrVectorAccordingToInputConfig(
        td, interval, typeCombination, ndv, seedArray, uniform, arrayLength);

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

void generateDataNormal(size_t size,
                        const std::pair<size_t, size_t>& interval,
                        const TypeCombination& typeCombination,
                        const size_t seed,
                        const size_t ndv,
                        std::vector<stats::SBEValue>& data,
                        int arrayLength) {
    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor normal{{stats::DistrType::kNormal, 1.0}};
    stats::TypeDistrVector td;

    populateTypeDistrVectorAccordingToInputConfig(
        td, interval, typeCombination, ndv, seedArray, normal, arrayLength);

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

void generateDataZipfian(const size_t size,
                         const std::pair<size_t, size_t>& interval,
                         const TypeCombination& typeCombination,
                         const size_t seed,
                         const size_t ndv,
                         std::vector<stats::SBEValue>& data,
                         int arrayLength) {
    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seed);

    stats::MixedDistributionDescriptor zipfian{{stats::DistrType::kZipfian, 1.0}};
    stats::TypeDistrVector td;

    populateTypeDistrVectorAccordingToInputConfig(
        td, interval, typeCombination, ndv, seedArray, zipfian, arrayLength);

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

std::vector<std::pair<stats::SBEValue, stats::SBEValue>> generateIntervals(
    QueryType queryType,
    const std::pair<size_t, size_t>& interval,
    size_t numberOfQueries,
    const TypeProbability& queryTypeInfo,
    size_t seedQueriesLow,
    size_t seedQueriesHigh) {
    std::vector<stats::SBEValue> sbeValLow, sbeValHigh;
    switch (queryType) {
        case kPoint: {
            // For ndv we set the number of values in the provided data interval. This may lead to
            // re-running values the same values if the number of queries is larger than the size of
            // the interval.
            auto ndv = interval.second - interval.first;
            if (queryTypeInfo.typeTag == sbe::value::TypeTags::StringSmall ||
                queryTypeInfo.typeTag == sbe::value::TypeTags::StringBig) {
                // Because 'interval' for strings is too small for 'ndv', set 'ndv' to
                // 'numberOfQueries' to ensure there are enough distinct values.
                ndv = numberOfQueries;
            }
            generateDataUniform(
                numberOfQueries, interval, {queryTypeInfo}, seedQueriesLow, ndv, sbeValLow);
            break;
        }
        case kRange: {
            const std::pair<size_t, size_t> intervalLow{interval.first, interval.second};

            const std::pair<size_t, size_t> intervalHigh{interval.first, interval.second};

            // For ndv we set the number of values in the provided data interval. This may lead to
            // re-running values the same values if the number of queries is larger than the size of
            // the interval.
            auto ndv = intervalLow.second - intervalLow.first;
            if (queryTypeInfo.typeTag == sbe::value::TypeTags::StringSmall ||
                queryTypeInfo.typeTag == sbe::value::TypeTags::StringBig) {
                // Because 'interval' for strings is too small for 'ndv', set 'ndv' to
                // 'numberOfQueries' to ensure there are enough distinct values.
                ndv = numberOfQueries;
            }
            generateDataUniform(
                numberOfQueries, intervalLow, {queryTypeInfo}, seedQueriesLow, ndv, sbeValLow);

            generateDataUniform(
                numberOfQueries, intervalHigh, {queryTypeInfo}, seedQueriesHigh, ndv, sbeValHigh);

            for (size_t i = 0; i < sbeValLow.size(); i++) {
                if (mongo::stats::compareValues(sbeValLow[i].getTag(),
                                                sbeValLow[i].getValue(),
                                                sbeValHigh[i].getTag(),
                                                sbeValHigh[i].getValue()) > 0) {
                    auto temp = sbeValHigh[i];
                    sbeValHigh[i] = sbeValLow[i];
                    sbeValLow[i] = temp;
                } else if (mongo::stats::compareValues(sbeValLow[i].getTag(),
                                                       sbeValLow[i].getValue(),
                                                       sbeValHigh[i].getTag(),
                                                       sbeValHigh[i].getValue()) == 0) {
                    // Remove elements from both vectors
                    sbeValLow.erase(sbeValLow.begin() + i);
                    sbeValHigh.erase(sbeValHigh.begin() + i);
                    i--;
                }
            }
            break;
        }
    }

    std::vector<std::pair<stats::SBEValue, stats::SBEValue>> intervals;
    for (size_t i = 0; i < sbeValLow.size(); ++i) {
        if (queryType == kPoint) {
            // Copy the first argument and move the second argument.
            intervals.emplace_back(sbeValLow[i], std::move(sbeValLow[i]));
        } else {
            intervals.emplace_back(std::move(sbeValLow[i]), std::move(sbeValHigh[i]));
        }
    }
    return intervals;
}

}  // namespace mongo::ce
