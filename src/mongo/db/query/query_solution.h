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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/stage_types.h"

namespace mongo {

    /**
     * This is an abstract representation of a query plan.  It can be transcribed into a tree of
     * PlanStages, which can then be handed to a PlanRunner for execution.
     */
    struct QuerySolutionNode {
        QuerySolutionNode() { }
        virtual ~QuerySolutionNode() { }

        /**
         * What stage should this be transcribed to?  See stage_types.h.
         */
        virtual StageType getType() const = 0;

        /**
         * Internal function called by toString()
         */
        virtual void appendToString(stringstream* ss) const = 0;
    private:
        MONGO_DISALLOW_COPYING(QuerySolutionNode);
    };

    /**
     * A QuerySolution must be entirely self-contained and own everything inside of it.
     *
     * A tree of stages may be built from a QuerySolution.  The QuerySolution must outlive the tree
     * of stages.
     */
    struct QuerySolution {
        QuerySolution() { }

        // Owned here.
        scoped_ptr<QuerySolutionNode> root;

        // Owned here.
        scoped_ptr<MatchExpression> filter;

        // Any filters in root or below point into this.  Must be owned.
        BSONObj filterData;

        /**
         * Output a human-readable string representing the plan.
         */
        string toString() {
            if (NULL == root) {
                return "empty query solution";
            }

            stringstream ss;
            root->appendToString(&ss);
            return ss.str();
        }
    private:
        MONGO_DISALLOW_COPYING(QuerySolution);
    };

    struct CollectionScanNode : public QuerySolutionNode {
        CollectionScanNode() : filter(NULL) { }

        virtual StageType getType() const { return STAGE_COLLSCAN; }

        virtual void appendToString(stringstream* ss) const {
            *ss << "COLLSCAN ns=" << name << " filter= " << filter->toString() << endl;
        }

        string name;

        // Not owned.
        // This is a sub-tree of the filter in the QuerySolution that owns us.
        MatchExpression* filter;
    };

}  // namespace mongo
