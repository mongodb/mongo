/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/metadata/path_arrayness_test_helpers.h"

#include "mongo/db/query/compiler/ce/ce_test_utils.h"

namespace mongo {

std::vector<std::pair<std::string, MultikeyComponents>> generateRandomFieldPathsWithArraynessInfo(
    int numberOfPaths,
    int maxLength,
    int ndvLengths,
    size_t seed,
    size_t seed2,
    std::pair<int, int> rangeFieldNameLength, /*default std::pair(1,4)*/
    TrieDepth trieDepth /*default TrieDepth::kMediumDepth*/) {

    std::pair<size_t, size_t> dataInterval = {1, maxLength};
    std::vector<stats::SBEValue> data;

    // Determine which distribution to use
    stats::DistrType distribution;
    bool invertForRightSkew = false;
    switch (trieDepth) {
        case TrieDepth::kShallow:
            // The Zipfian distribution is left skewed so this will produce more short field paths
            // than long and thus (on average) a shallower trie.
            distribution = stats::DistrType::kZipfian;
            break;
        case TrieDepth::kMediumDepth:
            // Field paths' lengths will be evenly distributed
            distribution = stats::DistrType::kUniform;
            break;
        case TrieDepth::kDeep:
            // Inverting the Zipfian distribution will make it right skewed and produce more long
            // field paths than short and thus (on average) a deeper trie.
            distribution = stats::DistrType::kZipfian;
            invertForRightSkew = true;
            break;
    }

    // Generate data according to the provided configuration
    ce::generateDataOneField(ndvLengths,
                             numberOfPaths,
                             {ce::parseCollectionType(sbe::value::TypeTags::NumberInt64)},
                             distribution,
                             dataInterval,
                             seed,
                             /*arrayTypeLength*/ 0,
                             data);

    // If right skew, invert the generated values
    if (invertForRightSkew) {
        for (auto& value : data) {
            tassert(11202201,
                    "Expected NumberInt64 type for path length values",
                    value.getTag() == sbe::value::TypeTags::NumberInt64);
            int64_t zipfianValue = sbe::value::bitcastTo<int64_t>(value.getValue());
            // Invert: max - zipfian + min to get right skew
            int64_t invertedValue = static_cast<int64_t>(dataInterval.second) - zipfianValue +
                static_cast<int64_t>(dataInterval.first);
            value = stats::SBEValue{stats::makeInt64Value(invertedValue)};
        }
    }

    std::vector<std::pair<std::string, MultikeyComponents>> pathsToInsert;
    for (const auto& length : data) {
        std::vector<stats::SBEValue> fieldNames;
        // Generate the strings for the fieldnames. Setting NDV as 5x the number of field paths to
        // generate, to increase variety.
        // dataInterval defines the length of the strings
        ce::generateDataOneField(
            /*ndv*/ length.getValue() * 5,
            /*size*/ length.getValue(),
            {ce::parseCollectionType(sbe::value::TypeTags::StringSmall)},
            /*dataDistribution*/ stats::DistrType::kUniform,
            rangeFieldNameLength,
            seed2,
            /*arrayTypeLength*/ 0,
            fieldNames);

        // Generate the arrayness of the individual fields randomly.
        std::vector<stats::SBEValue> fieldArrayness;
        ce::generateDataOneField(/*ndv*/ 2,
                                 /*size*/ length.getValue(),
                                 {ce::parseCollectionType(sbe::value::TypeTags::Boolean)},
                                 /*dataDistribution*/ stats::DistrType::kUniform,
                                 /*dataInterval*/ {0, 1},
                                 seed2,
                                 /*arrayTypeLength*/ 0,
                                 fieldArrayness);

        // Keep the state of multikeyness across field paths.
        stdx::unordered_map<std::string, bool> currentState;

        // The multikey components information for the current path.
        MultikeyComponents multikeyness;

        // The current path combined.
        std::stringstream fieldPath;

        int currentDepth = 0;
        for (const auto& fieldName : fieldNames) {
            // Add the dots in between.
            if (!fieldPath.str().empty()) {
                fieldPath << ".";
            }

            fieldPath << sbe::value::getStringView(fieldName.getTag(), fieldName.getValue());

            // Ensure we have consistency of arrayness across all field paths.
            auto alreadySeen = currentState.find(fieldPath.str());
            if (alreadySeen == currentState.find(fieldPath.str())) {
                currentState[fieldPath.str()] = fieldArrayness[currentDepth].getValue();
            }

            // If the field path is an array add to the multikey components.
            if (currentState[fieldPath.str()]) {
                multikeyness.insert(currentDepth);
            }
            currentDepth++;
        }

        pathsToInsert.push_back({fieldPath.str(), multikeyness});
    }

    return pathsToInsert;
}

std::vector<std::pair<std::string, MultikeyComponents>> combineVectors(
    const std::vector<FieldPath>& fieldPaths,
    const std::vector<MultikeyComponents>& multikeyComponents) {
    std::vector<std::pair<std::string, MultikeyComponents>> result;

    tassert(11202200,
            "The number of fieldpaths should be equal to the defs of multikeyness",
            fieldPaths.size() == multikeyComponents.size());

    for (size_t curPath = 0; curPath < fieldPaths.size(); curPath++) {
        result.push_back({fieldPaths[curPath].fullPath(), multikeyComponents[curPath]});
    }

    return result;
}

stdx::unordered_map<std::string, bool> tranformVectorToMap(
    const std::vector<std::pair<std::string, MultikeyComponents>>& vectorOfFieldPaths) {
    stdx::unordered_map<std::string, bool> result;

    for (const auto& curPair : vectorOfFieldPaths) {

        std::string s = curPair.first;

        std::vector<std::string> tokens;
        std::string delimiter = ".";
        size_t posStart = 0, posEnd;
        size_t delimLen = delimiter.length();
        std::string token;

        while ((posEnd = s.find(delimiter, posStart)) != std::string::npos) {
            token = s.substr(posStart, posEnd - posStart);
            posStart = posEnd + delimLen;
            tokens.push_back(token);
        }
        tokens.push_back(s.substr(posStart));  // Add the last token

        std::string builder = "";
        for (size_t i = 0; i < tokens.size(); i++) {
            builder += (builder.empty() ? tokens[i] : "." + tokens[i]);
            result[builder] = curPair.second.count(i);
        }
    }

    return result;
}

std::string truncatePathToLength(std::string path, size_t maxLength) {
    size_t length = 0;
    size_t lastDotIndex = 0;

    while (length < maxLength) {
        lastDotIndex = path.find('.', lastDotIndex + 1);
        length += 1;

        if (lastDotIndex == std::string::npos) {
            return path;
        }
    }

    // We want the path up until the index at which we either hit the end of the path or the
    // maximum desired length
    return path.substr(0, lastDotIndex);
}

}  // namespace mongo
