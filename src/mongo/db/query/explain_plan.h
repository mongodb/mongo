/**
 *    Copyright (C) 2013 10gen Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/exec/plan_stats.h"

namespace mongo {

    class TypeExplain;

    /**
     * Returns OK, allocating and filling in '*explain' describing the access paths used in
     * the 'stats' tree of a given query solution. The caller has the ownership of
     * '*explain', on success. Otherwise return an error status describing the problem.
     *
     * If 'fullDetails' was requested, the explain will return all available information about
     * the plan, otherwise, just a summary. The fields in the summary are: 'cursor', 'n',
     * 'nscannedObjects', 'nscanned', and 'indexBounds'. The remaining fields are: 'isMultKey',
     * 'nscannedObjectsAllPlans', 'nscannedAllPlans', 'scanAndOrder', 'indexOnly', 'nYields',
     * 'nChunkSkips', 'millis', 'allPlans', and 'oldPlan'.
     *
     * All these fields are documented in type_explain.h
     *
     * TODO: Currently, only working for single-leaf plans.
     */
    Status explainPlan(const PlanStageStats& stats, TypeExplain** explain, bool fullDetails);

    void statsToBSON(const PlanStageStats& stats, BSONObjBuilder* bob);

} // namespace mongo
