/*    Copyright 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>

namespace mongo {

/**
 * A container for the summary statistics that the profiler, slow query log, and
 * other non-explain debug mechanisms may want to collect.
 */
struct PlanSummaryStats {
    // The number of results returned by the plan.
    size_t nReturned = 0U;

    // The total number of index keys examined by the plan.
    size_t totalKeysExamined = 0U;

    // The total number of documents examined by the plan.
    size_t totalDocsExamined = 0U;

    // The number of milliseconds spent inside the root stage's work() method.
    long long executionTimeMillis = 0;

    // Did this plan use an in-memory sort stage?
    bool hasSortStage = false;

    // The names of each index used by the plan.
    std::set<std::string> indexesUsed;

    // Was this plan a result of using the MultiPlanStage to select a winner among several
    // candidates?
    bool fromMultiPlanner = false;

    // Was a replan triggered during the execution of this query?
    bool replanned = false;
};

}  // namespace mongo
