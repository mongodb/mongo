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
 */

#pragma once

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * The StageBuilder converts a QuerySolution to an executable tree of PlanStage(s).
     */
    class StageBuilder {
    public:
        /**
         * Turns 'solution' into an executable tree of PlanStage(s).
         *
         * Returns true if the PlanStage tree was built successfully.  The root of the tree is in
         * *rootOut and the WorkingSet that the tree uses is in *wsOut.
         *
         * Returns false otherwise.  *rootOut and *wsOut are invalid.
         */
        static bool build(const QuerySolution& solution, PlanStage** rootOut, WorkingSet** wsOut);
    };

}  // namespace mongo
