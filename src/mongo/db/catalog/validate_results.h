/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/record_id.h"

namespace mongo {

// Per-index validate results.
struct IndexValidateResults {
    bool valid = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    int64_t keysTraversed = 0;
    int64_t keysTraversedFromFullValidate = 0;
};

using ValidateResultsMap = std::map<std::string, IndexValidateResults>;

// Validation results for an entire collection.
struct ValidateResults {
    bool valid = true;
    bool repaired = false;
    boost::optional<Timestamp> readTimestamp = boost::none;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<BSONObj> extraIndexEntries;
    std::vector<BSONObj> missingIndexEntries;
    std::vector<RecordId> corruptRecords;
    long long numRemovedCorruptRecords = 0;
    long long numRemovedExtraIndexEntries = 0;
    long long numInsertedMissingIndexEntries = 0;

    // Maps index names to index-specific validation results.
    ValidateResultsMap indexResultsMap;

    // Takes a bool that indicates the context of the caller and a BSONObjBuilder to append with
    // validate results.
    void appendToResultObj(BSONObjBuilder& resultObj, bool debugging) const;
};

}  // namespace mongo
