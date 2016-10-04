/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/explain_common.h"

#include "mongo/util/mongoutils/str.h"

namespace mongo {

// static
const char* ExplainCommon::verbosityString(ExplainCommon::Verbosity verbosity) {
    switch (verbosity) {
        case QUERY_PLANNER:
            return "queryPlanner";
        case EXEC_STATS:
            return "executionStats";
        case EXEC_ALL_PLANS:
            return "allPlansExecution";
        default:
            invariant(0);
            return "unknown";
    }
}

// static
Status ExplainCommon::parseCmdBSON(const BSONObj& cmdObj, ExplainCommon::Verbosity* verbosity) {
    if (Object != cmdObj.firstElement().type()) {
        return Status(ErrorCodes::BadValue, "explain command requires a nested object");
    }

    *verbosity = ExplainCommon::EXEC_ALL_PLANS;
    if (!cmdObj["verbosity"].eoo()) {
        const char* verbStr = cmdObj["verbosity"].valuestrsafe();
        if (mongoutils::str::equals(verbStr, "queryPlanner")) {
            *verbosity = ExplainCommon::QUERY_PLANNER;
        } else if (mongoutils::str::equals(verbStr, "executionStats")) {
            *verbosity = ExplainCommon::EXEC_STATS;
        } else if (!mongoutils::str::equals(verbStr, "allPlansExecution")) {
            return Status(ErrorCodes::BadValue,
                          "verbosity string must be one of "
                          "{'queryPlanner', 'executionStats', 'allPlansExecution'}");
        }
    }

    return Status::OK();
}

}  // namespace mongo
