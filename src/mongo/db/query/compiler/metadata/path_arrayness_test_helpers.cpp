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
    int numberOfPaths, int maxLength, int ndvLengths, size_t seed, size_t seed2) {
    std::pair<size_t, size_t> dataInterval = {1, maxLength};

    std::vector<stats::SBEValue> data;
    // Generate data according to the provided configuration
    ce::generateDataOneField(ndvLengths,
                             numberOfPaths,
                             {ce::parseCollectionType(sbe::value::TypeTags::NumberInt64)},
                             /*dataDistribution*/ stats::DistrType::kUniform,
                             dataInterval,
                             seed,
                             /*arrayTypeLength*/ 0,
                             data);

    std::vector<std::pair<std::string, MultikeyComponents>> pathsToInsert;
    for (const auto& length : data) {
        std::vector<stats::SBEValue> fieldNames;
        // Generate the strings for the fieldnames. Setting NDV as 5x the number of field paths to
        // generate, to increase variety.
        // data interval defines the length of the strings (set currently between 1 and 4 character
        // length)
        ce::generateDataOneField(/*ndv*/ length.getValue() * 5,
                                 /*size*/ length.getValue(),
                                 {ce::parseCollectionType(sbe::value::TypeTags::StringSmall)},
                                 /*dataDistribution*/ stats::DistrType::kUniform,
                                 /*dataInterval*/ {1, 4},
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

}  // namespace mongo
