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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

/**
 * Represents options passed to the explain command (aside from the command which is being explained
 * and its parameters).
 */
class ExplainOptions {
public:
    /**
     * The various supported verbosity levels for explain. The order is significant: the enum values
     * are assigned in order of increasing verbosity.
     */
    enum class Verbosity {
        // At all verbosities greater than or equal to QUERY_PLANNER, we display information about
        // the plan selected and alternate rejected plans. Does not include any execution- related
        // info. String alias is "queryPlanner".
        kQueryPlanner = 0,

        // At all verbosities greater than or equal to EXEC_STATS, we display a section of output
        // containing both overall execution stats, and stats per stage in the execution tree.
        // String alias is "execStats".
        kExecStats = 1,

        // At this verbosity level, we generate the execution stats for each rejected plan as well
        // as the winning plan. String alias is "allPlansExecution".
        kExecAllPlans = 2,
    };

    static constexpr StringData kVerbosityName = "verbosity"_sd;

    // String representations for verbosity levels.
    static constexpr StringData kQueryPlannerVerbosityStr = "queryPlanner"_sd;
    static constexpr StringData kExecStatsVerbosityStr = "executionStats"_sd;
    static constexpr StringData kAllPlansExecutionVerbosityStr = "allPlansExecution"_sd;

    /**
     * Converts an explain verbosity to its string representation.
     */
    static StringData verbosityString(ExplainOptions::Verbosity verbosity);

    /**
     * Does some basic validation of the command BSON, then extracts and returns the explain
     * verbosity.
     *
     * Returns a non-OK status if parsing fails.
     */
    static StatusWith<ExplainOptions::Verbosity> parseCmdBSON(const BSONObj& cmdObj);

    /**
     * Converts 'verbosity' to its corresponding representation as a BSONObj containing explain
     * command parameters.
     */
    static BSONObj toBSON(ExplainOptions::Verbosity verbosity);
};

}  // namespace mongo
