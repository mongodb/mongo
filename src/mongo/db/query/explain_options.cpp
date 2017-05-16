/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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
    if (auto verbosityElt = cmdObj[kVerbosityName]) {
        if (verbosityElt.type() != BSONType::String) {
            return Status(ErrorCodes::FailedToParse, "explain verbosity must be a string");
        }

        auto verbStr = verbosityElt.valueStringData();
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
    }

    return verbosity;
}

BSONObj ExplainOptions::toBSON(ExplainOptions::Verbosity verbosity) {
    BSONObjBuilder explainOptionsBuilder;
    explainOptionsBuilder.append(kVerbosityName, StringData(verbosityString(verbosity)));
    return explainOptionsBuilder.obj();
}

}  // namespace mongo
