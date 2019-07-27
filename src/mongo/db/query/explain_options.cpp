/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/explain_options.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

constexpr StringData ExplainOptions::kVerbosityName;
constexpr StringData ExplainOptions::kQueryPlannerVerbosityStr;
constexpr StringData ExplainOptions::kExecStatsVerbosityStr;
constexpr StringData ExplainOptions::kAllPlansExecutionVerbosityStr;
const StringData kExplain = "explain";

StringData ExplainOptions::verbosityString(ExplainOptions::Verbosity verbosity) {
    switch (verbosity) {
    case Verbosity::kQueryPlanner:
        return kQueryPlannerVerbosityStr;
    case Verbosity::kExecStats:
        return kExecStatsVerbosityStr;
    case Verbosity::kExecAllPlans:
        return kAllPlansExecutionVerbosityStr;
    default:
        MONGO_UNREACHABLE;
    }
}

StatusWith<ExplainOptions::Verbosity> ExplainOptions::parseCmdBSON(const BSONObj& cmdObj) {
    if (BSONType::Object != cmdObj.firstElement().type()) {
        return Status(ErrorCodes::FailedToParse, "explain command requires a nested object");
    }

    auto verbosity = Verbosity::kExecAllPlans;
    for (auto && elem : cmdObj) {
        auto fieldName = elem.fieldNameStringData();
        auto verbStr = elem.valueStringData();

        // Assures the first the command is 'explain'.
        if (fieldName == kExplain) {
            continue;
        } else if (fieldName[0] == '$') {
            continue;
        }// Ignore top-level fields prefixed with $. They are for the command
        // processor, not us.
        else if (elem.type() != BSONType::String) {
            return Status(ErrorCodes::FailedToParse, "Field name must be a string");
        } else if (fieldName == kVerbosityName) {
            if (verbStr == kQueryPlannerVerbosityStr) {
                verbosity = Verbosity::kQueryPlanner;
            } else if (verbStr == kExecStatsVerbosityStr) {
                verbosity = Verbosity::kExecStats;
            } else if (verbStr != kAllPlansExecutionVerbosityStr) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "verbosity string must be one of {'"
                              << kQueryPlannerVerbosityStr
                              << "', '"
                              << kExecStatsVerbosityStr
                              << "', '"
                              << kAllPlansExecutionVerbosityStr
                              << "'}");
            }
        } else {
            return Status(ErrorCodes::FailedToParse, "Invalid commands");
        }
    }

    return verbosity;
}

BSONObj ExplainOptions::toBSON(ExplainOptions::Verbosity verbosity) {
    BSONObjBuilder explainOptionsBuilder;
    explainOptionsBuilder.append(kVerbosityName, StringData(verbosityString(verbosity)));
    return explainOptionsBuilder.obj();
}

}  // namespace mongo
