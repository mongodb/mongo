/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

std::string buildDottedPathFromFieldNames(std::vector<stats::SBEValue> fieldNames) {
    std::stringstream fieldPath;

    for (const auto& fieldName : fieldNames) {
        // Add the dots in between field names.
        if (!fieldPath.str().empty()) {
            fieldPath << ".";
        }

        fieldPath << sbe::value::getStringView(fieldName.getTag(), fieldName.getValue());
    }

    return fieldPath.str();
}

std::vector<std::string> generateRandomFieldPaths(int numberOfPaths,
                                                  int maxLength,
                                                  int ndvLengths,
                                                  size_t seed,
                                                  size_t seed2,
                                                  std::pair<int, int> rangeFieldNameLength) {

    std::pair<size_t, size_t> dataInterval = {1, maxLength};
    std::vector<stats::SBEValue> data;

    // Generate data according to the provided configuration
    ce::generateDataOneField(ndvLengths,
                             numberOfPaths,
                             {ce::parseCollectionType(sbe::value::TypeTags::NumberInt64)},
                             stats::DistrType::kUniform,
                             dataInterval,
                             seed,
                             /*arrayTypeLength*/ 0,
                             data);

    std::vector<std::string> fieldPaths;
    for (const auto& length : data) {
        std::vector<stats::SBEValue> fieldNames;
        // Generate the strings for the fieldnames. Setting NDV as 5x the number of field paths to
        // generate, to increase variety.
        // dataInterval defines the length of the strings.
        // arrayTypeLength is 0 because we are generating data of type StringSmall, so that
        // parameter is unused.
        ce::generateDataOneField(
            /*ndv*/ length.getValue() * 5,
            /*size*/ length.getValue(),
            {ce::parseCollectionType(sbe::value::TypeTags::StringSmall)},
            /*dataDistribution*/ stats::DistrType::kUniform,
            rangeFieldNameLength,
            seed2,
            /*arrayTypeLength*/ 0,
            fieldNames);

        fieldPaths.push_back(buildDottedPathFromFieldNames(fieldNames));
    }

    return fieldPaths;
}

std::vector<std::string> generateRandomFieldNames(int numberOfFields,
                                                  size_t seed,
                                                  std::pair<int, int> rangeFieldNameLength) {

    std::vector<stats::SBEValue> fieldNameValues;
    // Generate the strings for the fieldnames. Setting NDV as 5x the number of field names to
    // generate, to increase variety.
    ce::generateDataOneField(
        /*ndv*/ numberOfFields * 5,
        /*size*/ numberOfFields,
        {ce::parseCollectionType(sbe::value::TypeTags::StringSmall)},
        /*dataDistribution*/ stats::DistrType::kUniform,
        rangeFieldNameLength,
        seed,
        /*arrayTypeLength*/ 0,
        fieldNameValues);

    std::vector<std::string> fieldNames;
    fieldNames.reserve(numberOfFields);
    for (const auto& fieldName : fieldNameValues) {
        fieldNames.push_back(
            std::string(sbe::value::getStringView(fieldName.getTag(), fieldName.getValue())));
    }


    return fieldNames;
}

/**
 * Benchmarked operation: Calls FieldPath constructor with dotted path string passed in.
 */
void BM_FieldPathConstructor(benchmark::State& state) {
    size_t seed = 1354754;
    size_t seed2 = 3421354754;

    // Number of paths to insert.
    int numberOfPaths = static_cast<int>(state.range(0));

    // Number of distinct lengths of paths.
    int ndvLengths = numberOfPaths / 5;

    // Maximum length of dotted field paths.
    size_t maxLength = static_cast<size_t>(state.range(1));

    // Maximum length of each component of a dotted field path
    // The size of the range of possible lengths we choose from is 10 by default, and the bottom
    // bound must always be at least 1.
    int maxFieldNameLength = static_cast<int>(state.range(2));
    std::pair<int, int> rangeFieldNameLength(std::max(maxFieldNameLength - 10, 1),
                                             maxFieldNameLength);

    auto fieldPaths = generateRandomFieldPaths(
        numberOfPaths, maxLength, ndvLengths, seed, seed2, rangeFieldNameLength);

    for (auto _ : state) {
        for (size_t i = 0; i < fieldPaths.size(); i++) {
            auto constructedPath = FieldPath(fieldPaths[i]);
        }
    }
}

/**
 * Benchmarked operation: Calls FieldPath::validateFieldName() on field name strings.
 */
void BM_FieldPathValidateFieldName(benchmark::State& state) {
    size_t seed = 3421354754;

    // Number of paths to insert.
    int numberOfFields = static_cast<int>(state.range(0));

    // Maximum length of each field name.
    // The size of the range of possible lengths we choose from is 10 by default, and the bottom
    // bound must always be at least 1.
    int maxFieldNameLength = static_cast<int>(state.range(1));
    std::pair<int, int> rangeFieldNameLength(std::max(maxFieldNameLength - 10, 1),
                                             maxFieldNameLength);

    // Generate the field names
    auto fieldNames = generateRandomFieldNames(numberOfFields, seed, rangeFieldNameLength);

    for (auto _ : state) {
        for (size_t i = 0; i < fieldNames.size(); i++) {
            auto validationStatus = FieldPath::validateFieldName(fieldNames[i]);
        }
    }
}

BENCHMARK(BM_FieldPathConstructor)
    ->ArgNames({
        "numberOfPaths",
        "maxLength",
        "maxFieldNameLength",
    })
    ->ArgsProduct({
        /*numberOfPaths*/
        {64, 512, 1024, 2048},
        /*maxLength*/
        {10, 50, 100},
        /*maxFieldNameLength: */
        {5, 125, 250},
    })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.

BENCHMARK(BM_FieldPathValidateFieldName)
    ->ArgNames({
        "numberOfFields",
        "maxFieldNameLength",
    })
    ->ArgsProduct({
        /*numberOfFields*/
        {64, 512, 1024, 2048},
        /*maxFieldNameLength: */
        {5, 125, 250},
    })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.
}  // namespace mongo
