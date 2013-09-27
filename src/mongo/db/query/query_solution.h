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
#include "mongo/db/query/projection_parser.h"
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

        /**
         * If true, one of these are true:
         *          1. All outputs are already fetched, or
         *          2. There is a projection in place and a fetch is not required.
         *
         * If false, a fetch needs to be placed above the root in order to provide results.
         *
         * Usage: To determine if every possible result that might reach the root
         * will be fully-fetched or not.  We don't want any surplus fetches.
         */
        virtual bool fetched() const = 0;

        /**
         * Returns true if the tree rooted at this node provides data with the field name 'field'.
         * This data can come from any of the types of the WSM.
         *
         * Usage: If an index-only plan has all the fields we're interested in, we don't
         * have to fetch to show results with those fields.
         *
         * XXX TODO: Cover issues resulting from covered and multikey.  Multikey prohibits
         * covering, but allows sort (double check!)
         *
         * TODO: 'field' is probably more appropriate as a FieldRef or string.
         */
        virtual bool hasField(const string& field) const = 0;

        /**
         * Returns true if the tree rooted at this node provides data that is sorted by the
         * its location on disk.
         *
         * Usage: If all the children of an STAGE_AND_HASH have this property, we can compute the
         * AND faster by replacing the STAGE_AND_HASH with STAGE_AND_SORTED.
         */
        virtual bool sortedByDiskLoc() const = 0;

        /**
         * Return a BSONObj representing the sort order of the data stream from this node.  If the data
         * is not sorted in any particular fashion, returns BSONObj().
         *
         * TODO: Is BSONObj really the best way to represent this?
         *
         * Usage:
         * 1. If our plan gives us a sort order, we don't have to add a sort stage.
         * 2. If all the children of an OR have the same sort order, we can maintain that
         *    sort order with a STAGE_SORT_MERGE instead of STAGE_OR.
         */
        virtual BSONObj getSort() const = 0;

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
        QuerySolution() : hasSortStage(false) { }

        // Owned here.
        scoped_ptr<QuerySolutionNode> root;

        // Owned here.
        scoped_ptr<MatchExpression> filter;

        // Any filters in root or below point into this.  Must be owned.
        BSONObj filterData;

        string ns;

        // XXX temporary: if it has a sort stage the sort wasn't provided by an index,
        // so we use that index (if it exists) to provide a sort.
        bool hasSortStage;

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
        CollectionScanNode();
        virtual ~CollectionScanNode() { }

        virtual StageType getType() const { return STAGE_COLLSCAN; }

        virtual void appendToString(stringstream* ss, int indent) const;

        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return false; }
        BSONObj getSort() const { return BSONObj(); }

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
        AndHashNode();
        virtual ~AndHashNode();

        virtual StageType getType() const { return STAGE_AND_HASH; }

        virtual void appendToString(stringstream* ss, int indent) const;

        bool fetched() const;
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const { return false; }
        BSONObj getSort() const { return BSONObj(); }

        MatchExpression* filter;
        vector<QuerySolutionNode*> children;
    };

    struct OrNode : public QuerySolutionNode {
        OrNode();
        virtual ~OrNode();

        virtual StageType getType() const { return STAGE_OR; }

        virtual void appendToString(stringstream* ss, int indent) const;

        bool fetched() const;
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const {
            // Even if our children are sorted by their diskloc or other fields, we don't maintain
            // any order on the output.
            return false;
        }
        BSONObj getSort() const { return BSONObj(); }

        bool dedup;
        MatchExpression* filter;
        vector<QuerySolutionNode*> children;
    };

    struct FetchNode : public QuerySolutionNode {
        FetchNode();
        virtual ~FetchNode() { }

        virtual StageType getType() const { return STAGE_FETCH; }

        virtual void appendToString(stringstream* ss, int indent) const;

        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return child->sortedByDiskLoc(); }
        BSONObj getSort() const { return child->getSort(); }

        MatchExpression* filter;
        scoped_ptr<QuerySolutionNode> child;
    };

    struct IndexScanNode : public QuerySolutionNode {
        IndexScanNode();
        virtual ~IndexScanNode() { }

        virtual StageType getType() const { return STAGE_IXSCAN; }

        virtual void appendToString(stringstream* ss, int indent) const;

        bool fetched() const { return false; }
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const;
        BSONObj getSort() const { return indexKeyPattern; }

        BSONObj indexKeyPattern;

        // Not owned.
        // This is a sub-tree of the filter in the QuerySolution that owns us.
        // TODO: This may change in the future.
        MatchExpression* filter;

        // Only set for 2d.
        int limit;

        int direction;

        // BIG NOTE:
        // If you use simple bounds, we'll use whatever index access method the keypattern implies.
        // If you use the complex bounds, we force Btree access.
        // The complex bounds require Btree access.
        IndexBounds bounds;
    };

    struct ProjectionNode : public QuerySolutionNode {
        ProjectionNode() : projection(NULL) { }
        virtual ~ProjectionNode() { }

        virtual StageType getType() const { return STAGE_PROJECTION; }

        virtual void appendToString(stringstream* ss, int indent) const;

        /**
         * This node changes the type to OWNED_OBJ.  There's no fetching possible after this.
         */
        bool fetched() const { return true; }

        bool hasField(const string& field) const {
            // XXX XXX: perhaps have the QueryProjection pre-allocated and defer to it?  we don't
            // know what we're dropping.  Until we push projection down this doesn't matter.
            return false;
        }

        bool sortedByDiskLoc() const {
            // Projections destroy the DiskLoc.  By returning true here, this kind of implies that a
            // fetch could still be done upstream.
            //
            // Perhaps this should be false to not imply that there *is* a DiskLoc?  Kind of a
            // corner case.
            return child->sortedByDiskLoc();
        }

        BSONObj getSort() const {
            // TODO: If we're applying a projection that maintains sort order, the prefix of the
            // sort order we project is the sort order.
            return BSONObj();
        }

        // Points into the CanonicalQuery.
        ParsedProjection* projection;

        scoped_ptr<QuerySolutionNode> child;

        // TODO: Filter
    };

    struct SortNode : public QuerySolutionNode {
        SortNode() { }
        virtual ~SortNode() { }

        virtual StageType getType() const { return STAGE_SORT; }

        virtual void appendToString(stringstream* ss, int indent) const;
        
        bool fetched() const { return child->fetched(); }
        bool hasField(const string& field) const { return child->hasField(field); }
        bool sortedByDiskLoc() const { return false; }
        BSONObj getSort() const { return pattern; }

        BSONObj pattern;
        scoped_ptr<QuerySolutionNode> child;
        // TODO: Filter
    };

    struct LimitNode : public QuerySolutionNode {
        LimitNode() { }
        virtual ~LimitNode() { }

        virtual StageType getType() const { return STAGE_LIMIT; }

        virtual void appendToString(stringstream* ss, int indent) const;

        bool fetched() const { return child->fetched(); }
        bool hasField(const string& field) const { return child->hasField(field); }
        bool sortedByDiskLoc() const { return child->sortedByDiskLoc(); }
        BSONObj getSort() const { return child->getSort(); }

        int limit;
        scoped_ptr<QuerySolutionNode> child;
    };

    struct SkipNode : public QuerySolutionNode {
        SkipNode() { }
        virtual ~SkipNode() { }

        virtual StageType getType() const { return STAGE_SKIP; }

        virtual void appendToString(stringstream* ss, int indent) const;

        bool fetched() const { return child->fetched(); }
        bool hasField(const string& field) const { return child->hasField(field); }
        bool sortedByDiskLoc() const { return child->sortedByDiskLoc(); }
        BSONObj getSort() const { return child->getSort(); }

        int skip;
        scoped_ptr<QuerySolutionNode> child;
    };

}  // namespace mongo
