/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/db/query/plan_enumerator_explain_info.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/util/id_generator.h"

namespace mongo {

class GeoNearExpression;

/**
 * Represents the granularity at which a field is available in a query solution node. Note that the
 * order of the fields represents increasing availability.
 */
enum class FieldAvailability {
    // The field is not provided.
    kNotProvided,

    // The field is provided as a hash of raw data instead of the raw data itself. For example, this
    // can happen when the field is a hashed field in an index.
    kHashedValueProvided,

    // The field is completely provided.
    kFullyProvided,
};

std::ostream& operator<<(std::ostream& os, FieldAvailability value);

/**
 * Represents the set of sort orders satisfied by the data returned from a particular
 * QuerySolutionNode.
 */
class ProvidedSortSet {
public:
    ProvidedSortSet(BSONObj pattern, std::set<std::string> ignoreFields)
        : _baseSortPattern(std::move(pattern)), _ignoredFields(std::move(ignoreFields)) {}
    ProvidedSortSet(BSONObj pattern)
        : _baseSortPattern(std::move(pattern)), _ignoredFields(std::set<std::string>()) {}
    ProvidedSortSet() = default;

    /**
     * Returns true if the 'input' sort order is provided.
     *
     * Note: This function is sensitive to direction, i.e, if a pattern {a: 1} is provided, {a: -1}
     * may not be provided.
     */
    bool contains(BSONObj input) const;
    BSONObj getBaseSortPattern() const {
        return _baseSortPattern;
    }
    const std::set<std::string>& getIgnoredFields() const {
        return _ignoredFields;
    }
    std::string debugString() const {
        str::stream ss;
        ss << "baseSortPattern: " << _baseSortPattern << ", ignoredFields: [";
        for (auto&& ignoreField : _ignoredFields) {
            ss << ignoreField
               << /* last element */ (ignoreField == *_ignoredFields.rbegin() ? "" : ", ");
        }
        ss << "]";
        return ss;
    }

private:
    // The base sort order that is used as a reference to generate all possible sort orders. It is
    // also implied that all the prefixes of '_baseSortPattern' are provided.
    BSONObj _baseSortPattern;

    // Object to hold set of fields on which there is an equality predicate in the 'query' and
    // doesn't contribute to the sort order. Note that this doesn't include multiKey fields or
    // collations fields since they can contribute to the sort order.
    std::set<std::string> _ignoredFields;
};

std::ostream& operator<<(std::ostream& os, const ProvidedSortSet& value);
bool operator==(const ProvidedSortSet& lhs, const ProvidedSortSet& rhs);
bool operator!=(const ProvidedSortSet& lhs, const ProvidedSortSet& rhs);

/**
 * An empty ProvidedSortSet that can be used in QSNs that have no children and don't derive from
 * QuerySolutionNodeWithSortSet.
 */
inline const static ProvidedSortSet kEmptySet;

/**
 * This is an abstract representation of a query plan.  It can be transcribed into a tree of
 * PlanStages, which can then be handed to a PlanRunner for execution.
 */
struct QuerySolutionNode {
    QuerySolutionNode() = default;

    /**
     * Constructs a QuerySolutionNode with a single child.
     */
    QuerySolutionNode(std::unique_ptr<QuerySolutionNode> child) {
        children.push_back(std::move(child));
    }

    virtual ~QuerySolutionNode() = default;

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
    virtual void appendToString(str::stream* ss, int indent) const = 0;

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
     *          2. There is a stage in place that makes a FETCH stage unnecessary.
     *
     * If false, a fetch needs to be placed above the root in order to provide results.
     *
     * Usage: To determine if every possible result that might reach the root
     * will be fully-fetched or not.  We don't want any surplus fetches.
     */
    virtual bool fetched() const = 0;

    /**
     * Returns the granularity at which the tree rooted at this node provides data with the field
     * name 'field'. This data can come from any of the types of the WSM.
     *
     * Usage: If an index-only plan has all the fields we're interested in, we don't
     * have to fetch to show results with those fields.
     */
    virtual FieldAvailability getFieldAvailability(const std::string& field) const = 0;

    /**
     * Syntatic sugar on top of getFieldAvailability(). Returns true if the 'field' is fully
     * provided and false otherwise.
     */
    bool hasField(const std::string& field) const {
        return getFieldAvailability(field) == FieldAvailability::kFullyProvided;
    }

    /**
     * Returns true if the tree rooted at this node provides data that is sorted by its location on
     * disk.
     *
     * Usage: If all the children of an STAGE_AND_HASH have this property, we can compute the AND
     * faster by replacing the STAGE_AND_HASH with STAGE_AND_SORTED.
     */
    virtual bool sortedByDiskLoc() const = 0;

    /**
     * Returns a 'ProvidedSortSet' object which can be used to determine the possible sort orders of
     * the data returned from this node.
     *
     * Usage:
     * 1. If our plan gives us a sort order, we don't have to add a sort stage.
     * 2. If all the children of an OR have the same sort order, we can maintain that
     *    sort order with a STAGE_SORT_MERGE instead of STAGE_OR.
     */
    virtual const ProvidedSortSet& providedSorts() const = 0;

    /**
     * Make a deep copy.
     */
    virtual std::unique_ptr<QuerySolutionNode> clone() const = 0;

    /**
     * Adds a vector of query solution nodes to the list of children of this node.
     */
    void addChildren(std::vector<std::unique_ptr<QuerySolutionNode>> newChildren) {
        children.reserve(children.size() + newChildren.size());
        children.insert(children.end(),
                        std::make_move_iterator(newChildren.begin()),
                        std::make_move_iterator(newChildren.end()));
    }

    bool getScanLimit() {
        if (hitScanLimit) {
            return hitScanLimit;
        }
        for (const auto& child : children) {
            if (child->getScanLimit()) {
                hitScanLimit = true;
                return true;
            }
        }
        return false;
    }

    /**
     * True, if this node, or any of it's children is of the given 'type'.
     */
    bool hasNode(StageType type) const;

    /**
     * Returns the id associated with this node. Each node in a 'QuerySolution' tree is assigned a
     * unique identifier, which are assigned as sequential positive integers starting from 1.  An id
     * of 0 means that no id was explicitly assigned during construction of the QuerySolution.
     *
     * The identifiers are unique within the tree, but not across trees.
     */
    PlanNodeId nodeId() const {
        return _nodeId;
    }

    std::vector<std::unique_ptr<QuerySolutionNode>> children;

    // If a stage has a non-NULL filter all values outputted from that stage must pass that
    // filter.
    std::unique_ptr<MatchExpression> filter;

    bool hitScanLimit = false;

protected:
    /**
     * Formatting helper used by toString().
     */
    static void addIndent(str::stream* ss, int level);

    /**
     * Every solution node has properties and this adds the debug info for the
     * properties.
     */
    void addCommon(str::stream* ss, int indent) const;

    /**
     * Copy base query solution data from 'this' to 'other'.
     */
    void cloneBaseData(QuerySolutionNode* other) const {
        for (size_t i = 0; i < this->children.size(); i++) {
            other->children.push_back(this->children[i]->clone());
        }
        if (nullptr != this->filter) {
            other->filter = this->filter->shallowClone();
        }
    }

private:
    // Allows the QuerySolution constructor to set '_nodeId'.
    friend class QuerySolution;

    QuerySolutionNode(const QuerySolutionNode&) = delete;
    QuerySolutionNode& operator=(const QuerySolutionNode&) = delete;

    PlanNodeId _nodeId{0u};
};

struct QuerySolutionNodeWithSortSet : public QuerySolutionNode {
    QuerySolutionNodeWithSortSet() = default;

    /**
     * This constructor is only useful for QuerySolutionNodes with a single child.
     */
    explicit QuerySolutionNodeWithSortSet(std::unique_ptr<QuerySolutionNode> child)
        : QuerySolutionNode(std::move(child)) {}

    const ProvidedSortSet& providedSorts() const final {
        return sortSet;
    }

    void cloneBaseData(QuerySolutionNodeWithSortSet* other) const {
        QuerySolutionNode::cloneBaseData(other);
        other->sortSet = sortSet;
    }

    ProvidedSortSet sortSet;
};

/**
 * A QuerySolution must be entirely self-contained and own everything inside of it.
 *
 * A tree of stages may be built from a QuerySolution.  The QuerySolution must outlive the tree
 * of stages.
 */
class QuerySolution {
public:
    QuerySolution() = default;

    /**
     * Return true if this solution tree contains a node of the given 'type'.
     */
    bool hasNode(StageType type) const {
        return _root && _root->hasNode(type);
    }

    /**
     * Output a human-readable std::string representing the plan.
     */
    std::string toString() {
        if (!_root) {
            return "empty query solution";
        }

        str::stream ss;
        _root->appendToString(&ss, 0);
        return ss;
    }

    std::string summaryString() const;

    const QuerySolutionNode* root() const {
        return _root.get();
    }
    QuerySolutionNode* root() {
        return _root.get();
    }

    /**
     * Extends the solution's tree by attaching it to the tree rooted at 'extensionRoot'. The
     * extension tree must contain exactly one 'SentinelNode' node that denotes the attachment
     * point. The sentinel node will be replaces with the '_root' node.
     */
    void extendWith(std::unique_ptr<QuerySolutionNode> extensionRoot);

    /**
     * Assigns the QuerySolutionNode rooted at 'root' to this QuerySolution. Also assigns a unique
     * identifying integer to each node in the tree, which can subsequently be displayed in debug
     * output (e.g. explain).
     */
    void setRoot(std::unique_ptr<QuerySolutionNode> root);

    /**
     * Extracts the root of the QuerySolutionNode rooted at `_root`.
     */
    std::unique_ptr<QuerySolutionNode> extractRoot();

    /**
     * Returns a vector containing all of the secondary namespaces referenced by this tree, except
     * for 'mainNss'. This vector is used to track which secondary namespaces we should acquire
     * locks for. Note that the namespaces are returned in sorted order.
     */
    std::vector<NamespaceStringOrUUID> getAllSecondaryNamespaces(const NamespaceString& mainNss);

    // There are two known scenarios in which a query solution might potentially block:
    //
    // Sort stage:
    // If the solution has a sort stage, the sort wasn't provided by an index, so we might want
    // to scan an index to provide that sort in a non-blocking fashion.
    //
    // Hashed AND stage:
    // The hashed AND stage buffers data from multiple index scans and could block. In that case,
    // we would want to fall back on an alternate non-blocking solution.
    bool hasBlockingStage{false};

    // Indicates whether this query solution represents an 'explode for sort' plan when an index
    // scan over multiple point intervals is 'exploded' into a union of index scans in order to
    // obtain an indexed sort.
    bool hasExplodedForSort{false};

    // Runner executing this solution might be interested in knowing
    // if the planning process for this solution was based on filtered indices.
    bool indexFilterApplied{false};

    // Owned here. Used by the plan cache.
    std::unique_ptr<SolutionCacheData> cacheData;

    PlanEnumeratorExplainInfo _enumeratorExplainInfo;

    // Score calculated by PlanRanker. Only present if there are multiple candidate plans.
    boost::optional<double> score;

private:
    using QsnIdGenerator = IdGenerator<PlanNodeId>;

    QuerySolution(const QuerySolution&) = delete;
    QuerySolution& operator=(const QuerySolution&) = delete;

    void assignNodeIds(QsnIdGenerator& idGenerator, QuerySolutionNode& node);

    std::unique_ptr<QuerySolutionNode> _root;
};

struct CollectionScanNode : public QuerySolutionNodeWithSortSet {
    CollectionScanNode();
    virtual ~CollectionScanNode() {}

    virtual StageType getType() const {
        return STAGE_COLLSCAN;
    }

    virtual void computeProperties() override;
    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const {
        // It's possible this is overly conservative. By definition
        // a collection scan is sorted by its record ids, so if
        // we're scanning forward this might be true. However,
        // in practice this is only important for choosing between
        // hash and merge for index intersection.
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    // Name of the namespace.
    std::string name;

    // If present, this parameter sets the start point of a forward scan or the end point of a
    // reverse scan.
    boost::optional<RecordIdBound> minRecord;

    // If present, this parameter sets the start point of a reverse scan or the end point of a
    // forward scan.
    boost::optional<RecordIdBound> maxRecord;

    // If present, this parameter denotes the clustering info on the collection
    boost::optional<ClusteredIndexSpec> clusteredIndex;

    // Are the query and collection using the same collation?
    // Or are the bounds excluding situations where collation matters?
    bool hasCompatibleCollation;

    // If true, the collection scan will return a token that can be used to resume the scan.
    bool requestResumeToken = false;

    // If present, the collection scan will seek to the exact RecordId, or return KeyNotFound if it
    // does not exist. Must only be set on forward collection scans.
    // This field cannot be used in conjunction with 'minRecord' or 'maxRecord'.
    boost::optional<RecordId> resumeAfterRecordId;

    // Should we make a tailable cursor?
    bool tailable;

    // Should we keep track of the timestamp of the latest oplog entry we've seen? This information
    // is needed to merge cursors from the oplog in order of operation time when reading the oplog
    // across a sharded cluster.
    bool shouldTrackLatestOplogTimestamp = false;

    // Assert that the specified timestamp has not fallen off the oplog.
    boost::optional<Timestamp> assertTsHasNotFallenOff = boost::none;

    int direction{1};

    // By default, includes the minRecord and maxRecord when present.
    CollectionScanParams::ScanBoundInclusion boundInclusion =
        CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords;

    // Whether or not to wait for oplog visibility on oplog collection scans.
    bool shouldWaitForOplogVisibility = false;

    // Once the first matching document is found, assume that all documents after it must match.
    bool stopApplyingFilterAfterFirstMatch = false;
};

struct ColumnIndexScanNode : public QuerySolutionNode {
    ColumnIndexScanNode(ColumnIndexEntry,
                        OrderedPathSet outputFields,
                        OrderedPathSet matchFields,
                        StringMap<std::unique_ptr<MatchExpression>> filtersByPath,
                        std::unique_ptr<MatchExpression> postAssemblyFilter);

    virtual StageType getType() const {
        return STAGE_COLUMN_SCAN;
    }

    void appendToString(str::stream* ss, int indent) const override;

    bool fetched() const {
        return false;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return outputFields.find(field) != outputFields.end() ? FieldAvailability::kFullyProvided
                                                              : FieldAvailability::kNotProvided;
    }
    bool sortedByDiskLoc() const {
        return true;
    }

    const ProvidedSortSet& providedSorts() const {
        return kEmptySet;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final {
        StringMap<std::unique_ptr<MatchExpression>> clonedFiltersByPath;
        for (auto&& [path, filter] : filtersByPath) {
            clonedFiltersByPath[path] = filter->shallowClone();
        }
        return std::make_unique<ColumnIndexScanNode>(indexEntry,
                                                     outputFields,
                                                     matchFields,
                                                     std::move(clonedFiltersByPath),
                                                     postAssemblyFilter->shallowClone());
    }

    ColumnIndexEntry indexEntry;

    // The fields we need to output. Dot separated path names.
    OrderedPathSet outputFields;

    // The fields which are referenced by any and all filters - either in 'filtersByPath' or
    // 'postAssemblyFilter'.
    OrderedPathSet matchFields;

    // A column scan can apply a filter to the columns directly while scanning, or to a document
    // assembled from the scanned columns.

    // Filters to apply to a column directly while scanning. Maps the path to the filter for that
    // column. Empty if there are none.
    StringMap<std::unique_ptr<MatchExpression>> filtersByPath;

    // An optional filter to apply after assembling a document from all scanned columns. For
    // example: {$or: [{a: 2}, {b: 2}]}.
    std::unique_ptr<MatchExpression> postAssemblyFilter;

    // A cached copy of the union of the above two field sets which we expect to be frequently asked
    // for.
    OrderedPathSet allFields;
};

/**
 * A VirtualScanNode is similar to a collection or an index scan except that it doesn't depend on an
 * underlying storage implementation. It can be used to represent a virtual
 * collection or an index scan in memory by using a backing vector of BSONArray.
 */
struct VirtualScanNode : public QuerySolutionNodeWithSortSet {
    enum class ScanType { kCollScan, kIxscan };

    VirtualScanNode(std::vector<BSONArray> docs,
                    ScanType scanType,
                    bool hasRecordId,
                    BSONObj indexKeyPattern = {});

    virtual StageType getType() const {
        return STAGE_VIRTUAL_SCAN;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return scanType == ScanType::kCollScan;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        if (scanType == ScanType::kCollScan) {
            return FieldAvailability::kFullyProvided;
        } else {
            return indexKeyPattern.hasField(field) ? FieldAvailability::kFullyProvided
                                                   : FieldAvailability::kNotProvided;
        }
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    // A representation of a collection's documents. Here we use a BSONArray so metadata like a
    // RecordId can be stored alongside of the main document payload. The format of the data in
    // BSONArray is entirely up to a client of this node, but if this node is to be used for
    // consumption downstream by stage builder implementations it must conform to the format
    // expected by those stage builders. That expected contract depends on the hasRecordId flag. If
    // the hasRecordId flag is 'false' the BSONArray will have a single element that is a BSONObj
    // representation of a document being produced from this node. If 'hasRecordId' is true, then
    // each BSONArray in docs will carry a RecordId in the zeroth position of the array and a
    // BSONObj in the first position of the array.
    std::vector<BSONArray> docs;

    // Indicates whether the scan is mimicking a collection scan or index scan.
    const ScanType scanType;

    // A flag to indicate the format of the BSONArray document payload in the above vector, docs. If
    // hasRecordId is set to true, then both a RecordId and a BSONObj document are stored in that
    // order for every BSONArray in docs. Otherwise, the RecordId is omitted and the BSONArray will
    // only carry a BSONObj document.
    bool hasRecordId;

    // Set when 'scanType' is 'kIxscan'.
    BSONObj indexKeyPattern;
};

struct AndHashNode : public QuerySolutionNode {
    AndHashNode();
    virtual ~AndHashNode();

    virtual StageType getType() const {
        return STAGE_AND_HASH;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const;
    FieldAvailability getFieldAvailability(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return false;
    }
    const ProvidedSortSet& providedSorts() const {
        return children.back()->providedSorts();
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;
};

struct AndSortedNode : public QuerySolutionNodeWithSortSet {
    AndSortedNode();
    virtual ~AndSortedNode();

    virtual StageType getType() const {
        return STAGE_AND_SORTED;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const;
    FieldAvailability getFieldAvailability(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return true;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;
};

struct OrNode : public QuerySolutionNodeWithSortSet {
    OrNode();
    virtual ~OrNode();

    virtual StageType getType() const {
        return STAGE_OR;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const;
    FieldAvailability getFieldAvailability(const std::string& field) const;
    bool sortedByDiskLoc() const {
        // Even if our children are sorted by their diskloc or other fields, we don't maintain
        // any order on the output.
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const override;

    bool dedup;
};

struct MergeSortNode : public QuerySolutionNodeWithSortSet {
    MergeSortNode();
    virtual ~MergeSortNode();

    virtual StageType getType() const {
        return STAGE_SORT_MERGE;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const;
    FieldAvailability getFieldAvailability(const std::string& field) const;
    bool sortedByDiskLoc() const {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    virtual void computeProperties() {
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->computeProperties();
        }
        sortSet = ProvidedSortSet(sort, std::set<std::string>());
    }

    BSONObj sort;
    bool dedup;
};

struct FetchNode : public QuerySolutionNode {
    FetchNode() {}
    FetchNode(std::unique_ptr<QuerySolutionNode> child) : QuerySolutionNode(std::move(child)) {}
    virtual ~FetchNode() {}

    virtual StageType getType() const {
        return STAGE_FETCH;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const ProvidedSortSet& providedSorts() const {
        return children[0]->providedSorts();
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;
};

struct IndexScanNode : public QuerySolutionNodeWithSortSet {
    IndexScanNode(IndexEntry index);
    virtual ~IndexScanNode() {}

    virtual void computeProperties();

    virtual StageType getType() const {
        return STAGE_IXSCAN;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return false;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const;
    bool sortedByDiskLoc() const;

    std::unique_ptr<QuerySolutionNode> clone() const final;

    bool operator==(const IndexScanNode& other) const;

    /**
     * This function extracts a list of field names from 'indexKeyPattern' whose corresponding index
     * bounds in 'bounds' can contain strings.  This is the case if there are intervals containing
     * String, Object, or Array values.
     */
    static std::set<StringData> getFieldsWithStringBounds(const IndexBounds& bounds,
                                                          const BSONObj& indexKeyPattern);

    IndexEntry index;

    int direction;

    // If there's a 'returnKey' projection we add key metadata.
    bool addKeyMetadata;

    bool shouldDedup = false;

    IndexBounds bounds;

    const CollatorInterface* queryCollator;

    // The set of paths in the index key pattern which have at least one multikey path component, or
    // empty if the index either is not multikey or does not have path-level multikeyness metadata.
    //
    // The correct set of paths is computed and stored here by computeProperties().
    std::set<StringData> multikeyFields;

    /**
     * A vector of Interval Evaluation Trees (IETs) with the same ordering as the index key pattern.
     */
    std::vector<interval_evaluation_tree::IET> iets;
};

struct ReturnKeyNode : public QuerySolutionNode {
    ReturnKeyNode(std::unique_ptr<QuerySolutionNode> child,
                  std::vector<FieldPath> sortKeyMetaFields)
        : QuerySolutionNode(std::move(child)), sortKeyMetaFields(std::move(sortKeyMetaFields)) {}

    StageType getType() const final {
        return STAGE_RETURN_KEY;
    }

    void appendToString(str::stream* ss, int indent) const final;

    bool fetched() const final {
        return children[0]->fetched();
    }
    FieldAvailability getFieldAvailability(const std::string& field) const final {
        return FieldAvailability::kNotProvided;
    }
    bool sortedByDiskLoc() const final {
        return children[0]->sortedByDiskLoc();
    }
    const ProvidedSortSet& providedSorts() const final {
        return children[0]->providedSorts();
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    std::vector<FieldPath> sortKeyMetaFields;
};

/**
 * We have a few implementations of the projection functionality. They are chosen by constructing
 * a type derived from this abstract struct. The most general implementation 'ProjectionNodeDefault'
 * is much slower than the fast-path implementations. We only really have all the information
 * available to choose a projection implementation at planning time.
 */
struct ProjectionNode : public QuerySolutionNodeWithSortSet {
    ProjectionNode(std::unique_ptr<QuerySolutionNode> child,
                   const MatchExpression& fullExpression,
                   projection_ast::Projection proj)
        : QuerySolutionNodeWithSortSet(std::move(child)),
          fullExpression(fullExpression),
          proj(std::move(proj)) {}

    void computeProperties() final;

    void appendToString(str::stream* ss, int indent) const final;

    /**
     * Data from the projection node is considered fetch iff the child provides fetched data.
     */
    bool fetched() const {
        return children[0]->fetched();
    }

    FieldAvailability getFieldAvailability(const std::string& field) const {
        // If we were to construct a plan where the input to the project stage was a hashed value,
        // and that field was retained exactly, then we would mistakenly return 'kFullyProvided'.
        // The important point here is that we are careful to construct plans where we fetch before
        // projecting if there is hashed data, collation keys, etc. So this situation does not
        // arise.
        return proj.isFieldRetainedExactly(StringData{field}) ? FieldAvailability::kFullyProvided
                                                              : FieldAvailability::kNotProvided;
    }

    bool sortedByDiskLoc() const {
        // Projections destroy the RecordId.  By returning true here, this kind of implies that a
        // fetch could still be done upstream.
        //
        // Perhaps this should be false to not imply that there *is* a RecordId?  Kind of a
        // corner case.
        return children[0]->sortedByDiskLoc();
    }

protected:
    void cloneProjectionData(ProjectionNode* copy) const;

public:
    /**
     * Identify projectionImplementation type as a string.
     */
    virtual StringData projectionImplementationTypeToString() const = 0;

    // The full query tree.  Needed when we have positional operators.
    // Owned in the CanonicalQuery, not here.
    const MatchExpression& fullExpression;

    projection_ast::Projection proj;
};

/**
 * This is the most general implementation of the projection functionality. It handles every case.
 */
struct ProjectionNodeDefault final : ProjectionNode {
    using ProjectionNode::ProjectionNode;

    StageType getType() const final {
        return STAGE_PROJECTION_DEFAULT;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    StringData projectionImplementationTypeToString() const final {
        return "DEFAULT"_sd;
    }
};

/**
 * This is a fast-path for when the projection is fully covered by one index.
 */
struct ProjectionNodeCovered final : ProjectionNode {
    ProjectionNodeCovered(std::unique_ptr<QuerySolutionNode> child,
                          const MatchExpression& fullExpression,
                          projection_ast::Projection proj,
                          BSONObj coveredKeyObj)
        : ProjectionNode(std::move(child), fullExpression, std::move(proj)),
          coveredKeyObj(std::move(coveredKeyObj)) {}

    StageType getType() const final {
        return STAGE_PROJECTION_COVERED;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    StringData projectionImplementationTypeToString() const final {
        return "COVERED_ONE_INDEX"_sd;
    }

    // This is the key pattern of the index supplying our covered data. We can pre-compute which
    // fields to include and cache that data for later if we know we only have one index.
    BSONObj coveredKeyObj;
};

/**
 * This is a fast-path for when the projection only has inclusions on non-dotted fields.
 */
struct ProjectionNodeSimple final : ProjectionNode {
    using ProjectionNode::ProjectionNode;

    StageType getType() const final {
        return STAGE_PROJECTION_SIMPLE;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    StringData projectionImplementationTypeToString() const final {
        return "SIMPLE_DOC"_sd;
    }
};

struct SortKeyGeneratorNode : public QuerySolutionNode {
    StageType getType() const final {
        return STAGE_SORT_KEY_GENERATOR;
    }

    bool fetched() const final {
        return children[0]->fetched();
    }

    FieldAvailability getFieldAvailability(const std::string& field) const final {
        return children[0]->getFieldAvailability(field);
    }

    bool sortedByDiskLoc() const final {
        return children[0]->sortedByDiskLoc();
    }

    const ProvidedSortSet& providedSorts() const final {
        return children[0]->providedSorts();
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    void appendToString(str::stream* ss, int indent) const final;

    // The user-supplied sort pattern.
    BSONObj sortSpec;
};

struct SortNode : public QuerySolutionNodeWithSortSet {
    SortNode() : limit(0) {}

    virtual ~SortNode() {}

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return children[0]->getFieldAvailability(field);
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    virtual void computeProperties() {
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->computeProperties();
        }
        sortSet = ProvidedSortSet(pattern, std::set<std::string>());
    }

    BSONObj pattern;

    // Sum of both limit and skip count in the parsed query.
    size_t limit;

    bool addSortKeyMetadata = false;

    // The maximum number of bytes of memory we're willing to use during execution of the sort. If
    // this limit is exceeded and we're not allowed to spill to disk, the query will fail at
    // execution time. Otherwise, the data will be spilled to disk.
    uint64_t maxMemoryUsageBytes = internalQueryMaxBlockingSortMemoryUsageBytes.load();

protected:
    void cloneSortData(SortNode* copy) const;

private:
    virtual StringData sortImplementationTypeToString() const = 0;
};

/**
 * Represents sort algorithm that can handle any kind of input data.
 */
struct SortNodeDefault final : public SortNode {
    virtual StageType getType() const override {
        return STAGE_SORT_DEFAULT;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    StringData sortImplementationTypeToString() const override {
        return "DEFAULT"_sd;
    }
};

/**
 * Represents a special, optimized sort algorithm that is only correct if:
 *  - The input data is fetched.
 *  - The input data has no metadata attached.
 *  - The record id can be discarded.
 */
struct SortNodeSimple final : public SortNode {
    virtual StageType getType() const {
        return STAGE_SORT_SIMPLE;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    StringData sortImplementationTypeToString() const override {
        return "SIMPLE"_sd;
    }
};

struct LimitNode : public QuerySolutionNode {
    LimitNode() {}
    virtual ~LimitNode() {}

    virtual StageType getType() const {
        return STAGE_LIMIT;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return children[0]->getFieldAvailability(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const ProvidedSortSet& providedSorts() const {
        return children[0]->providedSorts();
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    long long limit;
};

struct SkipNode : public QuerySolutionNode {
    SkipNode() {}
    virtual ~SkipNode() {}

    virtual StageType getType() const {
        return STAGE_SKIP;
    }
    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return children[0]->getFieldAvailability(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const ProvidedSortSet& providedSorts() const {
        return children[0]->providedSorts();
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    long long skip;
};

struct GeoNear2DNode : public QuerySolutionNodeWithSortSet {
    GeoNear2DNode(IndexEntry index)
        : index(std::move(index)), addPointMeta(false), addDistMeta(false) {}

    virtual ~GeoNear2DNode() {}

    virtual StageType getType() const {
        return STAGE_GEO_NEAR_2D;
    }
    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    // Not owned here
    const GeoNearExpression* nq;
    IndexBounds baseBounds;

    IndexEntry index;
    bool addPointMeta;
    bool addDistMeta;
};

struct GeoNear2DSphereNode : public QuerySolutionNodeWithSortSet {
    GeoNear2DSphereNode(IndexEntry index)
        : index(std::move(index)), addPointMeta(false), addDistMeta(false) {}

    virtual ~GeoNear2DSphereNode() {}

    virtual StageType getType() const {
        return STAGE_GEO_NEAR_2DSPHERE;
    }
    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return true;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    // Not owned here
    const GeoNearExpression* nq;
    IndexBounds baseBounds;

    IndexEntry index;
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
    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return children[0]->fetched();
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return children[0]->getFieldAvailability(field);
    }
    bool sortedByDiskLoc() const {
        return children[0]->sortedByDiskLoc();
    }
    const ProvidedSortSet& providedSorts() const {
        return children[0]->providedSorts();
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;
};

/**
 * Distinct queries only want one value for a given field.  We run an index scan but
 * *always* skip over the current key to the next key.
 */
struct DistinctNode : public QuerySolutionNodeWithSortSet {
    DistinctNode(IndexEntry index) : index(std::move(index)) {}

    virtual ~DistinctNode() {}

    virtual StageType getType() const {
        return STAGE_DISTINCT_SCAN;
    }
    virtual void appendToString(str::stream* ss, int indent) const;

    // This stage is created "on top" of normal planning and as such the properties
    // below don't really matter.
    bool fetched() const {
        return false;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        // The distinct scan can return collation keys, but we can still consider the field fully
        // provided. This is because the logic around when the index bounds might incorporate
        // collation keys does not rely on 'getFieldAvailability()'. As a future improvement, we
        // could look into using 'getFieldAvailabilty()' for collation covering analysis.
        return index.keyPattern[field].eoo() ? FieldAvailability::kNotProvided
                                             : FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    virtual void computeProperties();

    IndexEntry index;
    IndexBounds bounds;

    const CollatorInterface* queryCollator;

    // We are distinct-ing over the 'fieldNo'-th field of 'index.keyPattern'.
    int fieldNo{0};
    int direction{1};
};

/**
 * Some count queries reduce to counting how many keys are between two entries in a
 * Btree.
 */
struct CountScanNode : public QuerySolutionNodeWithSortSet {
    CountScanNode(IndexEntry index) : index(std::move(index)) {}

    virtual ~CountScanNode() {}

    virtual StageType getType() const {
        return STAGE_COUNT_SCAN;
    }
    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return false;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    IndexEntry index;

    BSONObj startKey;
    bool startKeyInclusive;

    BSONObj endKey;
    bool endKeyInclusive;
};

struct EofNode : public QuerySolutionNodeWithSortSet {
    EofNode() {}

    virtual StageType getType() const {
        return STAGE_EOF;
    }

    virtual void appendToString(str::stream* ss, int indent) const;

    bool fetched() const {
        return false;
    }

    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kNotProvided;
    }

    bool sortedByDiskLoc() const {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;
};

struct TextOrNode : public OrNode {
    TextOrNode() {}

    StageType getType() const override {
        return STAGE_TEXT_OR;
    }

    void appendToString(str::stream* ss, int indent) const override;
    std::unique_ptr<QuerySolutionNode> clone() const final;
};

struct TextMatchNode : public QuerySolutionNodeWithSortSet {
    TextMatchNode(IndexEntry index, std::unique_ptr<fts::FTSQuery> ftsQuery, bool wantTextScore)
        : index(std::move(index)), ftsQuery(std::move(ftsQuery)), wantTextScore(wantTextScore) {}

    StageType getType() const override {
        return STAGE_TEXT_MATCH;
    }

    void appendToString(str::stream* ss, int indent) const override;

    // Text's return is LOC_AND_OBJ so it's fetched and has all fields.
    bool fetched() const {
        return true;
    }
    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const override {
        return false;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    IndexEntry index;
    std::unique_ptr<fts::FTSQuery> ftsQuery;

    // The number of fields in the prefix of the text index. For example, if the key pattern is
    //
    //   { a: 1, b: 1, _fts: "text", _ftsx: 1, c: 1 }
    //
    // then the number of prefix fields is 2, because of "a" and "b".
    size_t numPrefixFields = 0u;

    // "Prefix" fields of a text index can handle equality predicates.  We group them with the
    // text node while creating the text leaf node and convert them into a BSONObj index prefix
    // when we finish the text leaf node.
    BSONObj indexPrefix;

    // True, if we need to compute text scores.
    bool wantTextScore;
};

struct GroupNode : public QuerySolutionNode {
    GroupNode(std::unique_ptr<QuerySolutionNode> child,
              boost::intrusive_ptr<Expression> groupByExpression,
              std::vector<AccumulationStatement> accs,
              bool merging,
              bool shouldProduceBson)
        : QuerySolutionNode(std::move(child)),
          groupByExpression(groupByExpression),
          accumulators(std::move(accs)),
          doingMerge(merging),
          shouldProduceBson(shouldProduceBson) {
        // Use the DepsTracker to extract the fields that the 'groupByExpression' and accumulator
        // expressions depend on.
        for (auto& groupByExprField : groupByExpression->getDependencies().fields) {
            requiredFields.insert(groupByExprField);
        }
        for (auto&& acc : accumulators) {
            auto argExpr = acc.expr.argument;
            for (auto& argExprField : argExpr->getDependencies().fields) {
                requiredFields.insert(argExprField);
            }
        }
    }

    StageType getType() const override {
        return STAGE_GROUP;
    }

    void appendToString(str::stream* ss, int indent) const override;

    bool fetched() const {
        return true;
    }

    FieldAvailability getFieldAvailability(const std::string& field) const {
        // All fields are available, but none of them map to original document.
        return FieldAvailability::kNotProvided;
    }
    bool sortedByDiskLoc() const override {
        return false;
    }

    const ProvidedSortSet& providedSorts() const final {
        return kEmptySet;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    boost::intrusive_ptr<Expression> groupByExpression;
    std::vector<AccumulationStatement> accumulators;
    bool doingMerge;

    // Carries the fields this GroupNode depends on. Namely, 'requiredFields' contains the union of
    // the fields in the 'groupByExpressions' and the fields in the input Expressions of the
    // 'accumulators'.
    StringSet requiredFields;

    // If set to true, generated SBE plan will produce result as BSON object. If false,
    // 'sbe::Object' is produced instead.
    bool shouldProduceBson;
};

/**
 * Represents a lookup from a foreign collection by equality match on foreign and local fields.
 * Performs left outer join between the child (local) collection and other (foreign) collection.
 * Each local document will have a field with array of all matched documents in foreign collection.
 * Matching is performed using equality operator on specified fields in local and foreign documents.
 *
 * Only direct lookup from foreign collection is supported. Foreign collection is represented
 * by direct name rather than QuerySolutionNode.
 */
struct EqLookupNode : public QuerySolutionNode {
    /**
     * Enum describing the possible algorithms that can be used to execute a pushed down $lookup.
     */
    enum class LookupStrategy {
        // Execute the join by storing entries from the foreign collection in a hash table.
        kHashJoin,

        // Execute the join by doing an index lookup in the foreign collection.
        kIndexedLoopJoin,

        // Execute the join by iterating over the foreign collection for each local key.
        kNestedLoopJoin,

        // Create a plan for a non existent foreign collection.
        kNonExistentForeignCollection,
    };

    static StringData serializeLookupStrategy(LookupStrategy strategy) {
        switch (strategy) {
            case EqLookupNode::LookupStrategy::kHashJoin:
                return "HashJoin";
            case EqLookupNode::LookupStrategy::kIndexedLoopJoin:
                return "IndexedLoopJoin";
            case EqLookupNode::LookupStrategy::kNestedLoopJoin:
                return "NestedLoopJoin";
            case EqLookupNode::LookupStrategy::kNonExistentForeignCollection:
                return "NonExistentForeignCollection";
            default:
                uasserted(6357204, "Unknown $lookup strategy type");
        }
    }

    EqLookupNode(std::unique_ptr<QuerySolutionNode> child,
                 const NamespaceString& foreignCollection,
                 const FieldPath& joinFieldLocal,
                 const FieldPath& joinFieldForeign,
                 const FieldPath& joinField,
                 EqLookupNode::LookupStrategy lookupStrategy,
                 boost::optional<IndexEntry> idxEntry,
                 bool shouldProduceBson)
        : QuerySolutionNode(std::move(child)),
          foreignCollection(foreignCollection),
          joinFieldLocal(joinFieldLocal),
          joinFieldForeign(joinFieldForeign),
          joinField(joinField),
          lookupStrategy(lookupStrategy),
          idxEntry(std::move(idxEntry)),
          shouldProduceBson(shouldProduceBson) {}

    StageType getType() const override {
        return STAGE_EQ_LOOKUP;
    }

    void appendToString(str::stream* ss, int indent) const override;

    bool fetched() const {
        return children[0]->fetched();
    }

    FieldAvailability getFieldAvailability(const std::string& field) const {
        if (field == joinField) {
            // This field is available, but isn't mapped to the original document.
            return FieldAvailability::kNotProvided;
        } else {
            return children[0]->getFieldAvailability(field);
        }
    }

    bool sortedByDiskLoc() const override {
        return children[0]->sortedByDiskLoc();
    }

    const ProvidedSortSet& providedSorts() const final {
        // TODO SERVER-62815: The ProvidedSortSet will need to be computed here in order to allow
        // sort optimization. The "joinField" field overwrites the field in the result outer
        // document, this can affect the provided sort. For now, use conservative kEmptySet.
        return kEmptySet;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;

    /**
     * The foreign (inner) collection namespace string.
     */
    NamespaceString foreignCollection;

    /**
     * The local (outer) join field.
     */
    FieldPath joinFieldLocal;

    /**
     * The foreign (inner) join field.
     */
    FieldPath joinFieldForeign;

    /**
     * The "as" field for the output field that will be added to local (outer) document.
     * The field stores the array of all matched foreign (inner) documents.
     * If the field already exists in the local (outer) document, the field will be overwritten.
     */
    FieldPath joinField;

    /**
     * The algorithm that will be used to execute this 'EqLookupNode'. Defaults to nested loop join
     * as it's applicable independent of collection sizes or the availability of indexes.
     */
    LookupStrategy lookupStrategy = LookupStrategy::kNestedLoopJoin;

    /**
     * The index to be used if we can answer the join predicate with an index on the foreign
     * collection. Set to 'boost::none' by default and if a non-indexed strategy is chosen.
     */
    boost::optional<IndexEntry> idxEntry = boost::none;

    /**
     * If set to true, generated SBE plan will produce result as BSON object. If false,
     * 'sbe::Object' is produced instead.
     */
    bool shouldProduceBson;
};

struct SentinelNode : public QuerySolutionNode {

    SentinelNode() {}

    StageType getType() const override {
        return STAGE_SENTINEL;
    }

    void appendToString(str::stream* ss, int indent) const override;

    bool fetched() const {
        return true;
    }

    FieldAvailability getFieldAvailability(const std::string& field) const {
        return FieldAvailability::kFullyProvided;
    }
    bool sortedByDiskLoc() const override {
        return false;
    }

    const ProvidedSortSet& providedSorts() const final {
        return kEmptySet;
    }

    std::unique_ptr<QuerySolutionNode> clone() const final;
};
}  // namespace mongo
