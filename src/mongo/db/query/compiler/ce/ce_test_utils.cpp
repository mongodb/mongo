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

#include "mongo/db/query/compiler/ce/ce_test_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/query/compiler/stats/rand_utils_new.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::ce {

TypeProbability parseCollectionType(sbe::value::TypeTags dataType, bool nan) {
    if (nan) {
        return {sbe::value::TypeTags::NumberDouble, 100, 1.0};
    }

    switch (dataType) {
        case sbe::value::TypeTags::NumberInt64:
        case sbe::value::TypeTags::StringSmall:
        case sbe::value::TypeTags::StringBig:
        case sbe::value::TypeTags::NumberDouble:
        case sbe::value::TypeTags::Boolean:
        case sbe::value::TypeTags::Null:
            return {dataType, 100, 0.0};
        case sbe::value::TypeTags::Array:
            return {sbe::value::TypeTags::NumberDouble, 100, 0.0};
        default:
            MONGO_UNREACHABLE;
    };
}

std::pair<size_t, size_t> parseDataTypeToInterval(sbe::value::TypeTags dataType) {
    switch (dataType) {
        case sbe::value::TypeTags::NumberInt64:
            return {0, 1000};
        case sbe::value::TypeTags::StringSmall:
            // the data interval here represents the length of the string
            return {1, 8};
        case sbe::value::TypeTags::StringBig:
            // the data interval here represents the length of the string
            return {16, 32};
        case sbe::value::TypeTags::NumberDouble:
            return {0, 1000};
        case sbe::value::TypeTags::Boolean:
            return {0, 2};
        case sbe::value::TypeTags::Null:
            return {0, 1};
        case sbe::value::TypeTags::Array:
            return {0, 1000};
        default:
            MONGO_UNREACHABLE;
    }
}

void DataFieldDefinition::addToBSONObjBuilder(BSONObjBuilder& builder) const {
    builder << "fieldName" << fieldName;

    builder << "DataDistribution";
    switch (dataDistribution) {
        case stats::DistrType::kUniform:
            builder << "Uniform";
            break;
        case stats::DistrType::kNormal:
            builder << "Normal";
            break;
        case stats::DistrType::kZipfian:
            builder << "Zipfian";
            break;
    }

    builder << "TypeCombinationData";
    std::stringstream stringCombinationData;
    for (const auto& typeCombination : typeCombinationData) {
        stringCombinationData << typeCombination << ",";
    }
    builder << stringCombinationData.str();

    std::stringstream ndvstring;
    ndvstring << ndv;
    builder << "NDV" << ndvstring.str();

    // Inclusive minimum and maximum bounds for randomly generated data, ensuring each data
    // falls within these limits.
    std::stringstream stringDataInterval;
    stringDataInterval << dataInterval.first << ", " << dataInterval.second;
    builder << "DataInterval" << stringDataInterval.str();

    builder << "nanProb" << nanProb;

    std::stringstream stringArrayLength;
    stringArrayLength << arrayTypeLength;
    builder << "ArrayTypeLength" << stringArrayLength.str();

    std::stringstream stringSeed1;
    stringSeed1 << seed1;
    builder << "Seed1" << stringSeed1.str();

    std::stringstream stringSeed2;
    stringSeed2 << seed2;
    builder << "Seed2" << stringSeed2.str();
}

void CollectionFieldConfiguration::addToBSONObjBuilder(BSONObjBuilder& builder) const {
    builder << "fieldName" << fieldName;

    // Re-use parent builder.
    auto dataFieldDef = (DataFieldDefinition)(*this);
    dataFieldDef.addToBSONObjBuilder(builder);
}

void DataConfiguration::addToBSONObjBuilder(BSONObjBuilder& builder) const {
    std::stringstream stringSize;
    stringSize << size;
    builder << "Size" << stringSize.str();
    builder << "Fields";

    for (const auto& dataFieldConfig : collectionFieldsConfiguration) {
        dataFieldConfig.addToBSONObjBuilder(builder);
    }
}

void QueryConfiguration::addToBSONObjBuilder(BSONObjBuilder& builder) const {
    builder << "QueryFields";
    for (const auto& queryT : queryFields) {
        queryT.addToBSONObjBuilder(builder);
    }

    std::stringstream stringQueryTypes;
    for (const auto& queryT : queryTypes) {
        switch (queryT) {
            case kPoint:
                stringQueryTypes << "Point" << ",";
                break;
            case kRange:
                stringQueryTypes << "Range" << ",";
                break;
        }
    }
    builder << "QueryTypes" << stringQueryTypes.str();
}

void WorkloadConfiguration::addToBSONObjBuilder(BSONObjBuilder& builder) const {
    std::stringstream stringNumQueries;
    stringNumQueries << numberOfQueries;
    builder << "NumberOfQueries" << stringNumQueries.str();

    queryConfig.addToBSONObjBuilder(builder);
}

std::vector<BSONObj> transformSBEValueVectorToBSONObjVector(std::vector<stats::SBEValue> data,
                                                            std::string fieldName) {
    std::vector<BSONObj> result;
    for (const auto& temp : data) {
        result.push_back(stats::sbeValueToBSON(temp, fieldName));
    }
    return result;
}

std::vector<BSONObj> transformSBEValueVectorOfVectorsToBSONObjVector(
    std::vector<std::vector<stats::SBEValue>> data, std::vector<std::string> fieldNames) {
    std::vector<BSONObj> result;
    for (size_t idx = 0; idx < data.size(); idx++) {
        result.push_back(stats::sbeValueVectorToBSON(data[idx], fieldNames));
    }
    return result;
}

BSONObj createBSONObjOperandWithSBEValue(std::string str, stats::SBEValue value) {
    BSONObjBuilder builder;
    stats::addSbeValueToBSONBuilder(value, str, builder);
    return builder.obj();
}

std::unique_ptr<MatchExpression> createQueryMatchExpression(QueryType queryType,
                                                            const stats::SBEValue& sbeValLow,
                                                            const stats::SBEValue& sbeValHigh,
                                                            StringData fieldName) {
    switch (queryType) {
        case kPoint: {
            auto operand = createBSONObjOperandWithSBEValue("$eq", sbeValLow);
            auto eqExpr =
                std::make_unique<EqualityMatchExpression>(fieldName, mongo::Value(operand["$eq"]));
            return std::move(eqExpr);
        }
        case kRange: {
            auto operand1 = createBSONObjOperandWithSBEValue("$gte", sbeValLow);
            auto pred1 = std::make_unique<GTEMatchExpression>(fieldName, Value(operand1["$gte"]));
            auto operand2 = createBSONObjOperandWithSBEValue("$lte", sbeValHigh);
            auto pred2 = std::make_unique<LTMatchExpression>(fieldName, Value(operand2["$lte"]));
            auto andExpr = std::make_unique<AndMatchExpression>();
            andExpr->add(std::move(pred1));
            andExpr->add(std::move(pred2));
            return std::move(andExpr);
        }
    }
    MONGO_UNREACHABLE;
}

std::unique_ptr<MatchExpression> createQueryMatchExpression(
    std::vector<QueryType> queryTypes,
    std::vector<std::pair<stats::SBEValue, stats::SBEValue>> queryFieldIntervals,
    std::vector<DataFieldDefinition> fieldNames) {
    tassert(10624002,
            "Number of fields must match number of values",
            fieldNames.size() == queryFieldIntervals.size());
    tassert(10624003,
            "Number of fields must match number of query types",
            fieldNames.size() == queryTypes.size());
    auto fullExpr = std::make_unique<AndMatchExpression>();
    bool onlyOneExpr = (fieldNames.size() == 1);

    for (size_t fieldIdx = 0; fieldIdx < fieldNames.size(); fieldIdx++) {
        switch (queryTypes[fieldIdx]) {
            case kPoint: {
                auto operand =
                    createBSONObjOperandWithSBEValue("$eq", queryFieldIntervals[fieldIdx].first);
                auto eqExpr = std::make_unique<EqualityMatchExpression>(
                    StringData(fieldNames[fieldIdx].fieldName), mongo::Value(operand["$eq"]));

                if (onlyOneExpr) {
                    return std::move(eqExpr);
                }
                fullExpr->add(std::move(eqExpr));
                break;
            }
            case kRange: {
                auto operand1 =
                    createBSONObjOperandWithSBEValue("$gte", queryFieldIntervals[fieldIdx].first);
                auto pred1 = std::make_unique<GTEMatchExpression>(
                    StringData(fieldNames[fieldIdx].fieldName), Value(operand1["$gte"]));

                auto operand2 =
                    createBSONObjOperandWithSBEValue("$lte", queryFieldIntervals[fieldIdx].second);
                auto pred2 = std::make_unique<LTMatchExpression>(
                    StringData(fieldNames[fieldIdx].fieldName), Value(operand2["$lte"]));

                auto andExpr = std::make_unique<AndMatchExpression>();
                andExpr->add(std::move(pred1));
                andExpr->add(std::move(pred2));

                if (onlyOneExpr) {
                    return std::move(andExpr);
                }

                fullExpr->add(std::move(andExpr));
                break;
            }
        }
    }

    return std::move(fullExpr);
}

std::vector<std::unique_ptr<MatchExpression>> createQueryMatchExpressionOnMultipleFields(
    WorkloadConfiguration queryConfig,
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals) {
    std::vector<std::unique_ptr<MatchExpression>> allMatchExpressionQueries;
    for (size_t queryNumber = 0; queryNumber < queryConfig.numberOfQueries; queryNumber++) {
        std::vector<std::pair<stats::SBEValue, stats::SBEValue>> queryFieldsIntervalsPerQuery;
        for (size_t queryFieldIdx = 0; queryFieldIdx < queryConfig.queryConfig.queryFields.size();
             queryFieldIdx++) {
            queryFieldsIntervalsPerQuery.push_back(
                queryFieldsIntervals[queryFieldIdx][queryNumber]);
        }

        auto expr = createQueryMatchExpression(queryConfig.queryConfig.queryTypes,
                                               queryFieldsIntervalsPerQuery,
                                               queryConfig.queryConfig.queryFields);

        allMatchExpressionQueries.push_back(std::move(expr));
    }

    return allMatchExpressionQueries;
}

std::vector<std::unique_ptr<MatchExpression>> generateMatchExpressionsBasedOnWorkloadConfig(
    WorkloadConfiguration& workloadConfig) {
    // Generate queries.
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals =
        generateMultiFieldIntervals(workloadConfig);
    return createQueryMatchExpressionOnMultipleFields(workloadConfig, queryFieldsIntervals);
}

size_t calculateCardinality(const MatchExpression* expr, std::vector<BSONObj> data) {
    size_t cnt = 0;
    for (const auto& doc : data) {
        if (exec::matcher::matchesBSON(expr, doc, nullptr)) {
            cnt++;
        }
    }
    return cnt;
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

std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> generateMultiFieldIntervals(
    WorkloadConfiguration queryConfig) {
    std::vector<std::vector<std::pair<stats::SBEValue, stats::SBEValue>>> queryFieldsIntervals;
    for (size_t queryFieldIdx = 0; queryFieldIdx < queryConfig.queryConfig.queryFields.size();
         queryFieldIdx++) {
        // Generate intervals for this field
        auto queryFieldIntervals = generateIntervals(
            queryConfig.queryConfig.queryTypes[queryFieldIdx],
            queryConfig.queryConfig.queryFields[queryFieldIdx].dataInterval,
            queryConfig.numberOfQueries,
            queryConfig.queryConfig.queryFields[queryFieldIdx].typeCombinationData,
            queryConfig.queryConfig.queryFields[queryFieldIdx].seed1,
            queryConfig.queryConfig.queryFields[queryFieldIdx].seed2,
            queryConfig.queryConfig.queryFields[queryFieldIdx].ndv);

        queryFieldsIntervals.push_back(queryFieldIntervals);
    }

    return queryFieldsIntervals;
}

void generateDataOneField(size_t ndv,
                          size_t size,
                          TypeCombination typeCombinationData,
                          stats::DistrType dataDistribution,
                          const std::pair<size_t, size_t>& dataInterval,
                          size_t seedData,
                          int arrayTypeLength,
                          std::vector<stats::SBEValue>& data) {
    tassert(
        10545500, "For valid data generation number of distinct values (NDV) must be > 0", ndv > 0);
    // Random value generator for actual data in histogram.
    std::mt19937_64 seedArray(42);
    std::mt19937_64 seedDataset(seedData);

    stats::MixedDistributionDescriptor distr{{dataDistribution, 1.0}};
    stats::TypeDistrVector td;

    populateTypeDistrVectorAccordingToInputConfig(
        td, dataInterval, typeCombinationData, ndv, seedArray, distr, arrayTypeLength);

    stats::DatasetDescriptorNew desc{std::move(td), seedDataset};
    data = desc.genRandomDataset(size);
}

void generateDataBasedOnConfig(DataConfiguration& configuration,
                               std::vector<std::vector<stats::SBEValue>>& allData) {
    for (size_t i = 0; i < configuration.collectionFieldsConfiguration.size(); i++) {
        std::vector<stats::SBEValue> data;

        // Generate data according to the provided configuration
        generateDataOneField(configuration.collectionFieldsConfiguration[i].ndv,
                             configuration.size,
                             configuration.collectionFieldsConfiguration[i].typeCombinationData,
                             configuration.collectionFieldsConfiguration[i].dataDistribution,
                             configuration.collectionFieldsConfiguration[i].dataInterval,
                             configuration.collectionFieldsConfiguration[i].seed1,
                             configuration.collectionFieldsConfiguration[i].arrayTypeLength,
                             data);

        allData.push_back(data);
    }
}


std::vector<std::pair<stats::SBEValue, stats::SBEValue>> generateIntervals(
    QueryType queryType,
    const std::pair<size_t, size_t>& interval,
    size_t numberOfQueries,
    const TypeCombination& queryTypeInfo,
    size_t seedQueriesLow,
    size_t seedQueriesHigh,
    boost::optional<size_t> ndv_input) {
    std::vector<stats::SBEValue> sbeValLow, sbeValHigh;
    switch (queryType) {
        case kPoint: {
            // For ndv we set the number of values in the provided data interval. This may lead to
            // re-running values the same values if the number of queries is larger than the
            // size of the interval.
            auto ndv =
                (ndv_input.has_value()) ? ndv_input.value() : (interval.second - interval.first);

            generateDataOneField(ndv,
                                 numberOfQueries,
                                 queryTypeInfo,
                                 stats::DistrType::kUniform,
                                 interval,
                                 seedQueriesLow,
                                 /*arrayTypeLength*/ 0,
                                 sbeValLow);
            break;
        }
        case kRange: {
            const std::pair<size_t, size_t> intervalLow{interval.first, interval.second};
            const std::pair<size_t, size_t> intervalHigh{interval.first, interval.second};

            // For ndv we set the number of values in the provided data interval. This may lead to
            // re-running values the same values if the number of queries is larger than the
            // size of the interval.
            auto ndv =
                (ndv_input.has_value()) ? ndv_input.value() : (interval.second - interval.first);

            generateDataOneField(ndv,
                                 numberOfQueries,
                                 {queryTypeInfo},
                                 stats::DistrType::kUniform,
                                 intervalLow,
                                 seedQueriesLow,
                                 /*arrayTypeLength*/ 0,
                                 sbeValLow);

            generateDataOneField(ndv,
                                 numberOfQueries,
                                 {queryTypeInfo},
                                 stats::DistrType::kUniform,
                                 intervalHigh,
                                 seedQueriesHigh,
                                 /*arrayTypeLength*/ 0,
                                 sbeValHigh);

            for (size_t i = 0; i < sbeValLow.size(); i++) {
                if (mongo::stats::compareValues(sbeValLow[i].getTag(),
                                                sbeValLow[i].getValue(),
                                                sbeValHigh[i].getTag(),
                                                sbeValHigh[i].getValue()) >= 0) {
                    auto temp = sbeValHigh[i];
                    sbeValHigh[i] = sbeValLow[i];
                    sbeValLow[i] = temp;
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

bool checkTypeExistence(const sbe::value::TypeTags& checkType, const TypeCombination& typesInData) {
    bool typeExists = false;
    for (const auto& typeInSet : typesInData) {
        if (checkType == typeInSet.typeTag) {
            typeExists = true;
            break;
        } else if (typeInSet.typeTag == TypeTags::Array && checkType == TypeTags::NumberInt64) {
            // If the data type is array, we accept queries on integers. (the default data type in
            // arrays is integer.)
            typeExists = true;
            break;
        }
    }
    return typeExists;
}

}  // namespace mongo::ce
