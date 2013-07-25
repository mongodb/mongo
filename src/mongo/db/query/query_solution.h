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

#include "mongo/db/query/stage_types.h"

namespace mongo {

    /**
     * This is an abstract representation of a query plan.  It can be transcribed into a tree of
     * PlanStages, which can then be handed to a PlanRunner for execution.
     */
    struct QuerySolutionNode {
        virtual ~QuerySolutionNode() { }

        /**
         * What stage should this be transcribed to?  See stage_types.h.
         */
        virtual StageType getType() const = 0;

        /**
         * Output a human-readable string representing the plan.
         */
        string toString() {
            stringstream ss;
            appendToString(&ss);
            return ss.str();
        }

        /**
         * Internal function called by toString()
         */
        virtual void appendToString(stringstream* ss) const = 0;
    };

    // The root of the tree is the solution.
    typedef QuerySolutionNode QuerySolution;

    struct EmptyNode : public QuerySolutionNode {
        virtual StageType getType() const { return STAGE_UNKNOWN; }
        virtual void appendToString(stringstream* ss) const {
            *ss << "empty?!";
        }
    };

    // TODO: Implement.

}  // namespace mongo
