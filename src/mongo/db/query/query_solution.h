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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/stage_types.h"

namespace mongo {

    using mongo::fts::FTSQuery;

    /**
     * This is an abstract representation of a query plan.  It can be transcribed into a tree of
     * PlanStages, which can then be handed to a PlanRunner for execution.
     */
    struct QuerySolutionNode {
        QuerySolutionNode() { }
        virtual ~QuerySolutionNode() {
            for (size_t i = 0; i < children.size(); ++i) {
                delete children[i];
            }
        }

        /**
         * Return a string representation of this node and any children.
         */
        string toString() const;

        /**
         * What stage should this be transcribed to?  See stage_types.h.
         */
        virtual StageType getType() const = 0;

        /**
         * Internal function called by toString()
         *
         * TODO: Consider outputting into a BSONObj or builder thereof.
         */
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const = 0;

        //
        // Computed properties
        //

        /**
         * Must be called before any properties are examined.
         */
        virtual void computeProperties() {
            for (size_t i = 0; i < children.size(); ++i) {
                children[i]->computeProperties();
            }
        }

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
         * Return a BSONObjSet representing the possible sort orders of the data stream from this
         * node.  If the data is not sorted in any particular fashion, returns an empty set.
         *
         * Usage:
         * 1. If our plan gives us a sort order, we don't have to add a sort stage.
         * 2. If all the children of an OR have the same sort order, we can maintain that
         *    sort order with a STAGE_SORT_MERGE instead of STAGE_OR.
         */
        virtual const BSONObjSet& getSort() const = 0;

        // These are owned here.
        vector<QuerySolutionNode*> children;

        // If a stage has a non-NULL filter all values outputted from that stage must pass that
        // filter.
        scoped_ptr<MatchExpression> filter;

    protected:
        /**
         * Formatting helper used by toString().
         */
        static void addIndent(mongoutils::str::stream* ss, int level);

        /**
         * Every solution node has properties and this adds the debug info for the
         * properties.
         */
        void addCommon(mongoutils::str::stream* ss, int indent) const;

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

        // Any filters in root or below point into this object.  Must be owned.
        BSONObj filterData;

        string ns;

        // If the solution has a sort stage, the sort wasn't provided by an index, so we might want
        // to scan an index to provide that sort in a non-blocking fashion.
        bool hasSortStage;

        // Owned here. Used by the plan cache.
        boost::scoped_ptr<SolutionCacheData> cacheData;

        /**
         * Output a human-readable string representing the plan.
         */
        string toString() {
            if (NULL == root) {
                return "empty query solution";
            }

            mongoutils::str::stream ss;
            root->appendToString(&ss, 0);
            return ss;
        }
    private:
        MONGO_DISALLOW_COPYING(QuerySolution);
    };

    struct TextNode : public QuerySolutionNode {
        TextNode() { }
        virtual ~TextNode() { }

        virtual StageType getType() const { return STAGE_TEXT; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        // text's return is LOC_AND_UNOWNED_OBJ so it's fetched and has all fields.
        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return _sort; }

        BSONObjSet _sort;

        BSONObj  indexKeyPattern;
        std::string query;
        std::string language;

        // "Prefix" fields of a text index can handle equality predicates.  We group them with the
        // text node while creating the text leaf node and convert them into a BSONObj index prefix
        // when we finish the text leaf node.
        BSONObj indexPrefix;
    };

    struct CollectionScanNode : public QuerySolutionNode {
        CollectionScanNode();
        virtual ~CollectionScanNode() { }

        virtual StageType getType() const { return STAGE_COLLSCAN; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return _sort; }

        BSONObjSet _sort;

        // Name of the namespace.
        string name;

        // Should we make a tailable cursor?
        bool tailable;

        int direction;

        // maxScan option to .find() limits how many docs we look at.
        int maxScan;
    };

    struct AndHashNode : public QuerySolutionNode {
        AndHashNode();
        virtual ~AndHashNode();

        virtual StageType getType() const { return STAGE_AND_HASH; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const;
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return children.back()->getSort(); }

        BSONObjSet _sort;
    };

    struct AndSortedNode : public QuerySolutionNode {
        AndSortedNode();
        virtual ~AndSortedNode();

        virtual StageType getType() const { return STAGE_AND_SORTED; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const;
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const { return true; }
        const BSONObjSet& getSort() const { return _sort; }

        BSONObjSet _sort;
    };

    struct OrNode : public QuerySolutionNode {
        OrNode();
        virtual ~OrNode();

        virtual StageType getType() const { return STAGE_OR; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const;
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const {
            // Even if our children are sorted by their diskloc or other fields, we don't maintain
            // any order on the output.
            return false;
        }
        const BSONObjSet& getSort() const { return _sort; }

        BSONObjSet _sort;

        bool dedup;
    };

    struct MergeSortNode : public QuerySolutionNode {
        MergeSortNode();
        virtual ~MergeSortNode();

        virtual StageType getType() const { return STAGE_SORT_MERGE; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const;
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const { return false; }

        const BSONObjSet& getSort() const { return _sorts; }

        virtual void computeProperties() {
            for (size_t i = 0; i < children.size(); ++i) {
                children[i]->computeProperties();
            }
            _sorts.clear();
            _sorts.insert(sort);
        }

        BSONObjSet _sorts;

        BSONObj sort;
        bool dedup;
    };

    struct FetchNode : public QuerySolutionNode {
        FetchNode();
        virtual ~FetchNode() { }

        virtual StageType getType() const { return STAGE_FETCH; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return children[0]->sortedByDiskLoc(); }
        const BSONObjSet& getSort() const { return children[0]->getSort(); }

        BSONObjSet _sorts;
    };

    struct IndexScanNode : public QuerySolutionNode {
        IndexScanNode();
        virtual ~IndexScanNode() { }

        virtual void computeProperties();

        virtual StageType getType() const { return STAGE_IXSCAN; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return false; }
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const;
        const BSONObjSet& getSort() const { return _sorts; }

        BSONObjSet _sorts;

        BSONObj indexKeyPattern;
        bool indexIsMultiKey;

        int direction;

        // maxScan option to .find() limits how many docs we look at.
        int maxScan;

        // If there's a 'returnKey' projection we add key metadata.
        bool addKeyMetadata;

        // BIG NOTE:
        // If you use simple bounds, we'll use whatever index access method the keypattern implies.
        // If you use the complex bounds, we force Btree access.
        // The complex bounds require Btree access.
        IndexBounds bounds;
    };

    struct ProjectionNode : public QuerySolutionNode {
        ProjectionNode() { }
        virtual ~ProjectionNode() { }

        virtual StageType getType() const { return STAGE_PROJECTION; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

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
            return children[0]->sortedByDiskLoc();
        }

        const BSONObjSet& getSort() const {
            // TODO: If we're applying a projection that maintains sort order, the prefix of the
            // sort order we project is the sort order.
            return _sorts;
        }

        BSONObjSet _sorts;

        // The full query tree.  Needed when we have positional operators.
        // Owned in the CanonicalQuery, not here.
        MatchExpression* fullExpression;

        // Given that we don't yet have a MatchExpression analogue for the expression language, we
        // use a BSONObj.
        BSONObj projection;
    };

    struct SortNode : public QuerySolutionNode {
        SortNode() : limit(0) { }
        virtual ~SortNode() { }

        virtual StageType getType() const { return STAGE_SORT; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return children[0]->fetched(); }
        bool hasField(const string& field) const { return children[0]->hasField(field); }
        bool sortedByDiskLoc() const { return false; }

        const BSONObjSet& getSort() const { return _sorts; }

        virtual void computeProperties() {
            for (size_t i = 0; i < children.size(); ++i) {
                children[i]->computeProperties();
            }
            _sorts.clear();
            _sorts.insert(pattern);
        }

        BSONObjSet _sorts;

        BSONObj pattern;

        BSONObj query;

        // Sum of both limit and skip count in the parsed query.
        int limit;
    };

    struct LimitNode : public QuerySolutionNode {
        LimitNode() { }
        virtual ~LimitNode() { }

        virtual StageType getType() const { return STAGE_LIMIT; }

        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return children[0]->fetched(); }
        bool hasField(const string& field) const { return children[0]->hasField(field); }
        bool sortedByDiskLoc() const { return children[0]->sortedByDiskLoc(); }
        const BSONObjSet& getSort() const { return children[0]->getSort(); }

        int limit;
    };

    struct SkipNode : public QuerySolutionNode {
        SkipNode() { }
        virtual ~SkipNode() { }

        virtual StageType getType() const { return STAGE_SKIP; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return children[0]->fetched(); }
        bool hasField(const string& field) const { return children[0]->hasField(field); }
        bool sortedByDiskLoc() const { return children[0]->sortedByDiskLoc(); }
        const BSONObjSet& getSort() const { return children[0]->getSort(); }

        int skip;
    };

    //
    // Geo nodes.  A thin wrapper above an IXSCAN until we can yank functionality out of
    // the IXSCAN layer into the stage layer.
    //

    // TODO: This is probably an expression index.
    struct Geo2DNode : public QuerySolutionNode {
        Geo2DNode() { }
        virtual ~Geo2DNode() { }

        virtual StageType getType() const { return STAGE_GEO_2D; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return false; }
        bool hasField(const string& field) const;
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return _sorts; }
        BSONObjSet _sorts;

        BSONObj indexKeyPattern;
        GeoQuery gq;
    };

    // This is a standalone stage.
    struct GeoNear2DNode : public QuerySolutionNode {
        GeoNear2DNode() : numWanted(100), addPointMeta(false), addDistMeta(false) { }
        virtual ~GeoNear2DNode() { }

        virtual StageType getType() const { return STAGE_GEO_NEAR_2D; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return _sorts; }
        BSONObjSet _sorts;

        NearQuery nq;
        int numWanted;
        BSONObj indexKeyPattern;
        bool addPointMeta;
        bool addDistMeta;
    };

    // This is actually its own standalone stage.
    struct GeoNear2DSphereNode : public QuerySolutionNode {
        GeoNear2DSphereNode() : addPointMeta(false), addDistMeta(false) { }
        virtual ~GeoNear2DSphereNode() { }

        virtual StageType getType() const { return STAGE_GEO_NEAR_2DSPHERE; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return _sorts; }

        BSONObjSet _sorts;

        NearQuery nq;
        IndexBounds baseBounds;

        BSONObj indexKeyPattern;
        bool addPointMeta;
        bool addDistMeta;
    };

    //
    // Internal nodes used to provide functionality
    //

    /**
     * If we're answering a query on a sharded cluster, docs must be checked against the shard key
     * to ensure that we don't return data that shouldn't be there.  This must be done prior to
     * projection, and in fact should be done as early as possible to avoid propagating stale data
     * through the pipeline.
     */
    struct ShardingFilterNode : public QuerySolutionNode {
        ShardingFilterNode() { }
        virtual ~ShardingFilterNode() { }

        virtual StageType getType() const { return STAGE_SHARDING_FILTER; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return children[0]->fetched(); }
        bool hasField(const string& field) const { return children[0]->hasField(field); }
        bool sortedByDiskLoc() const { return children[0]->sortedByDiskLoc(); }
        const BSONObjSet& getSort() const { return children[0]->getSort(); }
    };

    /**
     * If documents mutate or are deleted during a query, we can (in some cases) fetch them
     * and still return them.  This stage merges documents that have been mutated or deleted
     * into the query result stream.
     */
    struct KeepMutationsNode : public QuerySolutionNode {
        KeepMutationsNode() { }
        virtual ~KeepMutationsNode() { }

        virtual StageType getType() const { return STAGE_KEEP_MUTATIONS; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        // Any flagged results are OWNED_OBJ and therefore we're covered if our child is.
        bool fetched() const { return children[0]->fetched(); }

        // Any flagged results are OWNED_OBJ and as such they'll have any field we need.
        bool hasField(const string& field) const { return children[0]->hasField(field); }

        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return sorts; }

        // Since we merge in flagged results we have no sort order.
        BSONObjSet sorts;
    };

    /**
     * Distinct queries only want one value for a given field.  We run an index scan but
     * *always* skip over the current key to the next key.
     */
    struct DistinctNode : public QuerySolutionNode {
        DistinctNode() { }
        virtual ~DistinctNode() { }

        virtual StageType getType() const { return STAGE_DISTINCT; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        // This stage is created "on top" of normal planning and as such the properties
        // below don't really matter.
        bool fetched() const { return true; }
        bool hasField(const string& field) const { return !indexKeyPattern[field].eoo(); }
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return sorts; }
        BSONObjSet sorts;

        BSONObj indexKeyPattern;
        int direction;
        IndexBounds bounds;
        // We are distinct-ing over the 'fieldNo'-th field of 'indexKeyPattern'.
        int fieldNo;
    };

    /**
     * Some count queries reduce to counting how many keys are between two entries in a
     * Btree.
     */
    struct CountNode : public QuerySolutionNode {
        CountNode() { }
        virtual ~CountNode() { }

        virtual StageType getType() const { return STAGE_COUNT; }
        virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

        bool fetched() const { return true; }
        bool hasField(const string& field) const { return true; }
        bool sortedByDiskLoc() const { return false; }
        const BSONObjSet& getSort() const { return sorts; }
        BSONObjSet sorts;

        BSONObj indexKeyPattern;

        BSONObj startKey;
        bool startKeyInclusive;

        BSONObj endKey;
        bool endKeyInclusive;
    };

}  // namespace mongo
