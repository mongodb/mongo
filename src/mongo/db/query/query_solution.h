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

#include <memory>

#include "mongo/db/fts/fts_query.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/stage_types.h"

namespace mongo {

class GeoNearExpression;

/**
 * This is an abstract representation of a query plan.  It can be transcribed into a tree of
 * PlanStages, which can then be handed to a PlanRunner for execution.
 */
struct QuerySolutionNode {
    QuerySolutionNode() {}
    virtual ~QuerySolutionNode() {
        for (size_t i = 0; i < children.size(); ++i) {
            delete children[i];
        }
    }

    /**
     * Return a std::string representation of this node and any children.
     */
    std::string toString() const;

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
    virtual bool hasField(const std::string& field) const = 0;

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

    /**
     * Make a deep copy.
     */
    virtual QuerySolutionNode* clone() const = 0;

    /**
     * Copy base query solution data from 'this' to 'other'.
     */
    void cloneBaseData(QuerySolutionNode* other) const {
        for (size_t i = 0; i < this->children.size(); i++) {
            other->children.push_back(this->children[i]->clone());
        }
        if (NULL != this->filter) {
            other->filter = this->filter->shallowClone();
        }
    }

    // These are owned here.
    std::vector<QuerySolutionNode*> children;

    // If a stage has a non-NULL filter all values outputted from that stage must pass that
    // filter.
    std::unique_ptr<MatchExpression> filter;

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
    QuerySolution() : hasBlockingStage(false), indexFilterApplied(false) {}

    // Owned here.
    std::unique_ptr<QuerySolutionNode> root;

    // Any filters in root or below point into this object.  Must be owned.
    BSONObj filterData;

    // There are two known scenarios in which a query solution might potentially block:
    //
    // Sort stage:
    // If the solution has a sort stage, the sort wasn't provided by an index, so we might want
    // to scan an index to provide that sort in a non-blocking fashion.
    //
    // Hashed AND stage:
    // The hashed AND stage buffers data from multiple index scans and could block. In that case,
    // we would want to fall back on an alternate non-blocking solution.
    bool hasBlockingStage;

    // Runner executing this solution might be interested in knowing
    // if the planning process for this solution was based on filtered indices.
    bool indexFilterApplied;

    // Owned here. Used by the plan cache.
    std::unique_ptr<SolutionCacheData> cacheData;

    /**
     * Output a human-readable std::string representing the plan.
     */
    std::string toString() {
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
    TextNode() {}
    virtual ~TextNode() {}

    virtual StageType getType() const {
        return STAGE_TEXT;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    // Text's return is LOC_AND_OBJ so it's fetched and has all fields.
    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;

    BSONObj indexKeyPattern;
    std::unique_ptr<fts::FTSQuery> ftsQuery;

    // "Prefix" fields of a text index can handle equality predicates.  We group them with the
    // text node while creating the text leaf node and convert them into a BSONObj index prefix
    // when we finish the text leaf node.
    BSONObj indexPrefix;
};

struct CollectionScanNode : public QuerySolutionNode {
    CollectionScanNode();
    virtual ~CollectionScanNode() {}

    virtual StageType getType() const {
        return STAGE_COLLSCAN;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;

    // Name of the namespace.
    std::string name;

    // Should we make a tailable cursor?
    bool tailable;

    int direction;

    // maxScan option to .find() limits how many docs we look at.
    int maxScan;
};

struct AndHashNode : public QuerySolutionNode {
    AndHashNode();
    virtual ~AndHashNode();

    virtual StageType getType() const {
        return STAGE_AND_HASH;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return children.back()->getSort();
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;
};

struct AndSortedNode : public QuerySolutionNode {
    AndSortedNode();
    virtual ~AndSortedNode();

    virtual StageType getType() const {
        return STAGE_AND_SORTED;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return true;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;
};

struct OrNode : public QuerySolutionNode {
    OrNode();
    virtual ~OrNode();

    virtual StageType getType() const {
        return STAGE_OR;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        // Even if our children are sorted by their diskloc or other fields, we don't maintain
        // any order on the output.
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sort;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sort;

    bool dedup;
};

struct MergeSortNode : public QuerySolutionNode {
    MergeSortNode();
    virtual ~MergeSortNode();

    virtual StageType getType() const {
        return STAGE_SORT_MERGE;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const;
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return false;
    }

    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

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
    virtual ~FetchNode() {}

    virtual StageType getType() const {
        return STAGE_FETCH;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;
};

struct IndexScanNode : public QuerySolutionNode {
    IndexScanNode();
    virtual ~IndexScanNode() {}

    virtual void computeProperties();

    virtual StageType getType() const {
        return STAGE_IXSCAN;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return false;
    }
    bool hasField(const std::string& field) const;
    bool sortedByDiskLoc() const;
    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    bool operator==(const IndexScanNode& other) const;

    /**
     * This function extracts a list of field names from 'indexKeyPattern' whose corresponding index
     * bounds in 'bounds' can contain strings.  This is the case if there are intervals containing
     * String, Object, or Array values.
     */
    static std::set<StringData> getFieldsWithStringBounds(const IndexBounds& bounds,
                                                          const BSONObj& indexKeyPattern);

    BSONObjSet _sorts;

    BSONObj indexKeyPattern;
    bool indexIsMultiKey;

    int direction;

    // maxScan option to .find() limits how many docs we look at.
    int maxScan;

    // If there's a 'returnKey' projection we add key metadata.
    bool addKeyMetadata;

    IndexBounds bounds;

    const CollatorInterface* indexCollator;
    const CollatorInterface* queryCollator;
};

struct ProjectionNode : public QuerySolutionNode {
    /**
     * We have a few implementations of the projection functionality.  The most general
     * implementation 'DEFAULT' is much slower than the fast-path implementations
     * below.  We only really have all the information available to choose a projection
     * implementation at planning time.
     */
    enum ProjectionType {
        // This is the most general implementation of the projection functionality.  It handles
        // every case.
        DEFAULT,

        // This is a fast-path for when the projection is fully covered by one index.
        COVERED_ONE_INDEX,

        // This is a fast-path for when the projection only has inclusions on non-dotted fields.
        SIMPLE_DOC,
    };

    ProjectionNode(ParsedProjection proj) : fullExpression(NULL), projType(DEFAULT), parsed(proj) {}

    virtual ~ProjectionNode() {}

    virtual StageType getType() const {
        return STAGE_PROJECTION;
    }

    virtual void computeProperties();

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    /**
     * Data from the projection node is considered fetch iff the child provides fetched data.
     */
    bool fetched() const {
        return children[0]->fetched();
    }

    bool hasField(const std::string& field) const {
        // TODO: Returning false isn't always the right answer -- we may either be including
        // certain fields, or we may be dropping fields (in which case hasField returns true).
        //
        // Given that projection sits on top of everything else in .find() it doesn't matter
        // what we do here.
        return false;
    }

    bool sortedByDiskLoc() const {
        // Projections destroy the RecordId.  By returning true here, this kind of implies that a
        // fetch could still be done upstream.
        //
        // Perhaps this should be false to not imply that there *is* a RecordId?  Kind of a
        // corner case.
        return children[0]->sortedByDiskLoc();
    }

    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;

    // The full query tree.  Needed when we have positional operators.
    // Owned in the CanonicalQuery, not here.
    MatchExpression* fullExpression;

    // Given that we don't yet have a MatchExpression analogue for the expression language, we
    // use a BSONObj.
    BSONObj projection;

    // What implementation of the projection algorithm should we use?
    ProjectionType projType;

    ParsedProjection parsed;

    // Only meaningful if projType == COVERED_ONE_INDEX.  This is the key pattern of the index
    // supplying our covered data.  We can pre-compute which fields to include and cache that
    // data for later if we know we only have one index.
    BSONObj coveredKeyObj;
};

struct SortKeyGeneratorNode : public QuerySolutionNode {
    StageType getType() const final {
        return STAGE_SORT_KEY_GENERATOR;
    }

    bool fetched() const final {
        return children[0]->fetched();
    }

    bool hasField(const std::string& field) const final {
        return children[0]->hasField(field);
    }

    bool sortedByDiskLoc() const final {
        return children[0]->sortedByDiskLoc();
    }

    const BSONObjSet& getSort() const final {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const final;

    void appendToString(mongoutils::str::stream* ss, int indent) const final;

    // The query predicate provided by the user. For sorted by an array field, the sort key depends
    // on the predicate.
    BSONObj queryObj;

    // The user-supplied sort pattern.
    BSONObj sortSpec;
};

struct SortNode : public QuerySolutionNode {
    SortNode() : limit(0) {}
    virtual ~SortNode() {}

    virtual StageType getType() const {
        return STAGE_SORT;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    virtual void computeProperties() {
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->computeProperties();
        }
        _sorts.clear();
        _sorts.insert(pattern);
    }

    BSONObjSet _sorts;

    BSONObj pattern;

    // Sum of both limit and skip count in the parsed query.
    size_t limit;
};

struct LimitNode : public QuerySolutionNode {
    LimitNode() {}
    virtual ~LimitNode() {}

    virtual StageType getType() const {
        return STAGE_LIMIT;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    long long limit;
};

struct SkipNode : public QuerySolutionNode {
    SkipNode() {}
    virtual ~SkipNode() {}

    virtual StageType getType() const {
        return STAGE_SKIP;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    long long skip;
};

// This is a standalone stage.
struct GeoNear2DNode : public QuerySolutionNode {
    GeoNear2DNode() : addPointMeta(false), addDistMeta(false) {}
    virtual ~GeoNear2DNode() {}

    virtual StageType getType() const {
        return STAGE_GEO_NEAR_2D;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;

    // Not owned here
    const GeoNearExpression* nq;
    IndexBounds baseBounds;

    BSONObj indexKeyPattern;
    bool addPointMeta;
    bool addDistMeta;
};

// This is actually its own standalone stage.
struct GeoNear2DSphereNode : public QuerySolutionNode {
    GeoNear2DSphereNode() : addPointMeta(false), addDistMeta(false) {}
    virtual ~GeoNear2DSphereNode() {}

    virtual StageType getType() const {
        return STAGE_GEO_NEAR_2DSPHERE;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return _sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet _sorts;

    // Not owned here
    const GeoNearExpression* nq;
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
    ShardingFilterNode() {}
    virtual ~ShardingFilterNode() {}

    virtual StageType getType() const {
        return STAGE_SHARDING_FILTER;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;
};

/**
 * If documents mutate or are deleted during a query, we can (in some cases) fetch them
 * and still return them.  This stage merges documents that have been mutated or deleted
 * into the query result stream.
 */
struct KeepMutationsNode : public QuerySolutionNode {
    KeepMutationsNode() {}
    virtual ~KeepMutationsNode() {}

    virtual StageType getType() const {
        return STAGE_KEEP_MUTATIONS;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    // Any flagged results are OWNED_OBJ and therefore we're covered if our child is.
    bool fetched() const {
        return children[0]->fetched();
    }

    // Any flagged results are OWNED_OBJ and as such they'll have any field we need.
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }

    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return sorts;
    }

    QuerySolutionNode* clone() const;

    // Since we merge in flagged results we have no sort order.
    BSONObjSet sorts;
};

/**
 * Distinct queries only want one value for a given field.  We run an index scan but
 * *always* skip over the current key to the next key.
 */
struct DistinctNode : public QuerySolutionNode {
    DistinctNode() {}
    virtual ~DistinctNode() {}

    virtual StageType getType() const {
        return STAGE_DISTINCT_SCAN;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    // This stage is created "on top" of normal planning and as such the properties
    // below don't really matter.
    bool fetched() const {
        return false;
    }
    bool hasField(const std::string& field) const {
        return !indexKeyPattern[field].eoo();
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return sorts;
    }

    QuerySolutionNode* clone() const;

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
struct CountScanNode : public QuerySolutionNode {
    CountScanNode() {}
    virtual ~CountScanNode() {}

    virtual StageType getType() const {
        return STAGE_COUNT_SCAN;
    }
    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return false;
    }
    bool hasField(const std::string& field) const {
        return true;
    }
    bool sortedByDiskLoc() const {
        return false;
    }
    const BSONObjSet& getSort() const {
        return sorts;
    }

    QuerySolutionNode* clone() const;

    BSONObjSet sorts;

    BSONObj indexKeyPattern;

    BSONObj startKey;
    bool startKeyInclusive;

    BSONObj endKey;
    bool endKeyInclusive;
};

/**
 * This stage drops results that are out of sorted order.
 */
struct EnsureSortedNode : public QuerySolutionNode {
    EnsureSortedNode() {}
    virtual ~EnsureSortedNode() {}

    virtual StageType getType() const {
        return STAGE_ENSURE_SORTED;
    }

    virtual void appendToString(mongoutils::str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    bool hasField(const std::string& field) const {
        return children[0]->hasField(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const BSONObjSet& getSort() const {
        return children[0]->getSort();
    }

    QuerySolutionNode* clone() const;

    // The pattern that the results should be sorted by.
    BSONObj pattern;
};

}  // namespace mongo
