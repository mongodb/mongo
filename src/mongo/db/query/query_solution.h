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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_bounds.h"
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
         *
         * TODO: Consider outputting into a BSONObj or builder thereof.
         */
        virtual void appendToString(stringstream* ss, int indent) const = 0;

    protected:
        static void addIndent(stringstream* ss, int level) {
            for (int i = 0; i < level; ++i) {
                *ss << "---";
            }
        }

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

        string ns;

        /**
         * Output a human-readable string representing the plan.
         */
        string toString() {
            if (NULL == root) {
                return "empty query solution";
            }

            stringstream ss;
            root->appendToString(&ss, 0);
            return ss.str();
        }
    private:
        MONGO_DISALLOW_COPYING(QuerySolution);
    };

    struct CollectionScanNode : public QuerySolutionNode {
        CollectionScanNode() : tailable(false), direction(1), filter(NULL) { }

        virtual StageType getType() const { return STAGE_COLLSCAN; }

        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "COLLSCAN";
            addIndent(ss, indent + 1);
            *ss <<  "ns = " << name;
            if (NULL != filter) {
                addIndent(ss, indent + 1);
                *ss << " filter = " << filter->toString() << endl;
            }
        }

        // Name of the namespace.
        string name;

        // Should we make a tailable cursor?
        bool tailable;

        int direction;

        // Not owned.
        // This is a sub-tree of the filter in the QuerySolution that owns us.
        // TODO: This may change in the future.
        MatchExpression* filter;
    };

    struct AndHashNode : public QuerySolutionNode {
        AndHashNode() : filter(NULL) { }
        ~AndHashNode() {
            for (size_t i = 0; i < children.size(); ++i) {
                delete children[i];
            }
        }
        virtual StageType getType() const { return STAGE_AND_HASH; }
        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "AND_HASH";
            if (NULL != filter) {
                addIndent(ss, indent + 1);
                *ss << " filter = " << filter->toString() << endl;
            }
            for (size_t i = 0; i < children.size(); ++i) {
                *ss << "Child " << i << ": ";
                children[i]->appendToString(ss, indent + 1);
            }
        }
        MatchExpression* filter;
        vector<QuerySolutionNode*> children;
    };

    struct OrNode : public QuerySolutionNode {
        OrNode() : dedup(true), filter(NULL) { }
        ~OrNode() {
            for (size_t i = 0; i < children.size(); ++i) {
                delete children[i];
            }
        }
        virtual StageType getType() const { return STAGE_OR; }
        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "OR\n";
            if (NULL != filter) {
                addIndent(ss, indent + 1);
                *ss << " filter = " << filter->toString() << endl;
            }
            for (size_t i = 0; i < children.size(); ++i) {
                addIndent(ss, indent + 1);
                *ss << "Child " << i << ":\n";
                children[i]->appendToString(ss, indent + 2);
                *ss << endl;
            }
        }
        bool dedup;
        MatchExpression* filter;
        vector<QuerySolutionNode*> children;
    };

    struct FetchNode : public QuerySolutionNode {
        FetchNode() : filter(NULL) { }
        virtual StageType getType() const { return STAGE_FETCH; }
        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "FETCH\n";
            if (NULL != filter) {
                addIndent(ss, indent + 1);
                StringBuilder sb;
                *ss << "filter:\n";
                filter->debugString(sb, indent + 2);
                *ss << sb.str();
            }
            addIndent(ss, indent + 1);
            *ss << "Child:" << endl;
            child->appendToString(ss, indent + 2);
        }
        MatchExpression* filter;
        scoped_ptr<QuerySolutionNode> child;
    };

    struct IndexScanNode : public QuerySolutionNode {
        IndexScanNode() : filter(NULL), limit(0), direction(1) { }

        virtual StageType getType() const { return STAGE_IXSCAN; }

        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "IXSCAN\n";
            addIndent(ss, indent + 1);
            *ss << "keyPattern = " << indexKeyPattern << endl;
            if (NULL != filter) {
                addIndent(ss, indent + 1);
                *ss << " filter= " << filter->toString() << endl;
            }
            addIndent(ss, indent + 1);
            *ss << "dir = " << direction << endl;
            addIndent(ss, indent + 1);
            *ss << "bounds = " << bounds.toString();
        }

        BSONObj indexKeyPattern;

        // Not owned.
        // This is a sub-tree of the filter in the QuerySolution that owns us.
        // TODO: This may change in the future.
        MatchExpression* filter;

        // Only set for 2d.
        int limit;

        int direction;

        IndexBounds bounds;
    };

    struct ProjectionNode : public QuerySolutionNode {
        ProjectionNode() { }
        virtual StageType getType() const { return STAGE_PROJECTION; }

        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "PROJ\n";
            addIndent(ss, indent + 1);
            *ss << "proj = " << projection.toString() << endl;
            addIndent(ss, indent + 1);
            *ss << "Child:" << endl;
            child->appendToString(ss, indent + 2);
        }

        BSONObj projection;
        scoped_ptr<QuerySolutionNode> child;
        // TODO: Filter
    };

    struct SortNode : public QuerySolutionNode {
        SortNode() { }
        virtual StageType getType() const { return STAGE_SORT; }

        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "SORT\n";
            addIndent(ss, indent + 1);
            *ss << "pattern = " << pattern.toString() << endl;
            addIndent(ss, indent + 1);
            *ss << "Child:" << endl;
            child->appendToString(ss, indent + 2);
        }

        BSONObj pattern;
        scoped_ptr<QuerySolutionNode> child;
        // TODO: Filter
    };

    struct LimitNode : public QuerySolutionNode {
        LimitNode() { }
        virtual StageType getType() const { return STAGE_LIMIT; }

        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "LIMIT\n";
            addIndent(ss, indent + 1);
            *ss << "limit = " << limit << endl;
            addIndent(ss, indent + 1);
            *ss << "Child:" << endl;
            child->appendToString(ss, indent + 2);
        }

        int limit;
        scoped_ptr<QuerySolutionNode> child;
    };

    struct SkipNode : public QuerySolutionNode {
        SkipNode() { }
        virtual StageType getType() const { return STAGE_SKIP; }

        virtual void appendToString(stringstream* ss, int indent) const {
            addIndent(ss, indent);
            *ss << "SKIP\n";
            addIndent(ss, indent + 1);
            *ss << "skip= " << skip << endl;
            addIndent(ss, indent + 1);
            *ss << "Child:" << endl;
            child->appendToString(ss, indent + 2);
        }

        int skip;
        scoped_ptr<QuerySolutionNode> child;
    };

}  // namespace mongo
