/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"


namespace mongo::optimizer {

using FilterType = ABT;
using ProjectionType = ABT;

/**
 * Marker for node class (both logical and physical sub-classes).
 * A node not marked with either ExclusivelyLogicalNode or ExclusivelyPhysicalNode is considered to
 * be both a logical and a physical node (e.g. a filter node). It is invalid to mark a node with
 * both tags at the same time.
 */
class Node {};

/**
 * Marker for exclusively logical nodes.
 */
class ExclusivelyLogicalNode : public Node {};

/**
 * Marker for exclusively physical nodes.
 */
class ExclusivelyPhysicalNode : public Node {};

inline void assertNodeSort(const ABT& e) {
    tassert(6624009, "Node syntax sort expected", e.is<Node>());
}

template <class T>
inline constexpr bool canBeLogicalNode() {
    // Node which is not exclusively physical.
    return std::is_base_of_v<Node, T> && !std::is_base_of_v<ExclusivelyPhysicalNode, T>;
}

template <class T>
inline constexpr bool canBePhysicalNode() {
    // Node which is not exclusively logical.
    return std::is_base_of_v<Node, T> && !std::is_base_of_v<ExclusivelyLogicalNode, T>;
}

/**
 * Logical Scan node.
 * Represents scanning from an underlying collection and producing a single projection conceptually
 * containing the stream of BSON objects read from the collection.
 */
class ScanNode final : public Operator<1>, public ExclusivelyLogicalNode {
    using Base = Operator<1>;

public:
    static constexpr const char* kDefaultCollectionNameSpec = "collectionName";

    ScanNode(ProjectionName projectionName, std::string scanDefName);

    bool operator==(const ScanNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<0>();
        tassert(6624010, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ProjectionName& getProjectionName() const;
    const ProjectionType& getProjection() const;

    const std::string& getScanDefName() const;

private:
    const std::string _scanDefName;
};

/**
 * Physical Scan node.
 * It defines scanning a collection with an optional projection name that contains the documents.
 *
 * Optionally set of fields is specified to retrieve from the underlying collection, and expose as
 * projections.
 */
class PhysicalScanNode final : public Operator<1>, public ExclusivelyPhysicalNode {
    using Base = Operator<1>;

public:
    PhysicalScanNode(FieldProjectionMap fieldProjectionMap,
                     std::string scanDefName,
                     bool useParallelScan);

    bool operator==(const PhysicalScanNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<0>();
        tassert(6624011, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const FieldProjectionMap& getFieldProjectionMap() const;

    const std::string& getScanDefName() const;

    bool useParallelScan() const;

private:
    const FieldProjectionMap _fieldProjectionMap;
    const std::string _scanDefName;
    const bool _useParallelScan;
};

/**
 * Logical ValueScanNode.
 *
 * It originates a set of projections each with a fixed sequence of values, which is encoded as an
 * array. Each array element has as many entries as the number of projections plus one. If are
 * providing a row id, the first one must be of type RecordId.
 */
class ValueScanNode final : public Operator<1>, public ExclusivelyLogicalNode {
    using Base = Operator<1>;

public:
    ValueScanNode(ProjectionNameVector projections,
                  boost::optional<properties::LogicalProps> props);

    /**
     * Each element of 'valueArray' is an array itself and must have one entry corresponding to
     * each of 'projections'.
     */
    ValueScanNode(ProjectionNameVector projections,
                  boost::optional<properties::LogicalProps> props,
                  ABT valueArray,
                  bool hasRID);

    bool operator==(const ValueScanNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<0>();
        tassert(6624012, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ABT& getValueArray() const;
    size_t getArraySize() const;

    const boost::optional<properties::LogicalProps>& getProps() const;

    bool getHasRID() const;

private:
    // Optional logical properties. Used as a seed during logical proeprties derivation.
    const boost::optional<properties::LogicalProps> _props;

    const ABT _valueArray;
    size_t _arraySize;

    // Indicates if the valueArray provides a column with RecordId elements.
    const bool _hasRID;
};

/**
 * Physical CoScanNode.
 *
 * The "Co" in CoScan indicates that it is constant; conceptually it originates an infinite stream
 * of Nothing. A typical use case is to limit it to one document, and attach projections with a
 * following EvaluationNode(s).
 */
class CoScanNode final : public Operator<0>, public ExclusivelyPhysicalNode {
    using Base = Operator<0>;

public:
    CoScanNode();

    bool operator==(const CoScanNode& other) const;
};

/**
 * Index scan node.
 * Retrieve data using an index. Return recordIds or values (if the index is covering).
 * This is a physical node.
 */
class IndexScanNode final : public Operator<1>, public ExclusivelyPhysicalNode {
    using Base = Operator<1>;

public:
    IndexScanNode(FieldProjectionMap fieldProjectionMap, IndexSpecification indexSpec);

    bool operator==(const IndexScanNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<0>();
        tassert(6624013, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const FieldProjectionMap& getFieldProjectionMap() const;
    const IndexSpecification& getIndexSpecification() const;

private:
    const FieldProjectionMap _fieldProjectionMap;
    const IndexSpecification _indexSpec;
};

/**
 * SeekNode.
 * Retrieve values using rowIds (typically previously retrieved using an index scan).
 * This is a physical node.
 *
 * 'ridProjectionName' parameter designates the incoming rid which is the starting point of the
 * seek. 'fieldProjectionMap' may choose to include an outgoing rid which will contain the
 * successive (if we do not have a following limit) document ids.
 *
 * TODO SERVER-68936: Can we let it advance with a limit based on upper rid limit in case of primary
 * index?
 */
class SeekNode final : public Operator<2>, public ExclusivelyPhysicalNode {
    using Base = Operator<2>;

public:
    SeekNode(ProjectionName ridProjectionName,
             FieldProjectionMap fieldProjectionMap,
             std::string scanDefName);

    bool operator==(const SeekNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<0>();
        tassert(6624014, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ProjectionName& getRIDProjectionName() const;

    const FieldProjectionMap& getFieldProjectionMap() const;

    const std::string& getScanDefName() const;

private:
    const ProjectionName _ridProjectionName;
    const FieldProjectionMap _fieldProjectionMap;
    const std::string _scanDefName;
};


/**
 * Logical group delegator node: scan from a given group.
 * Used in conjunction with memo.
 */
class MemoLogicalDelegatorNode final : public Operator<0>, public ExclusivelyLogicalNode {
    using Base = Operator<0>;

public:
    MemoLogicalDelegatorNode(GroupIdType groupId);

    bool operator==(const MemoLogicalDelegatorNode& other) const;

    GroupIdType getGroupId() const;

private:
    const GroupIdType _groupId;
};

/**
 * Physical group delegator node: refer to a physical node in a memo group.
 * Used in conjunction with memo.
 */
class MemoPhysicalDelegatorNode final : public Operator<0>, public ExclusivelyPhysicalNode {
    using Base = Operator<0>;

public:
    MemoPhysicalDelegatorNode(MemoPhysicalNodeId nodeId);

    bool operator==(const MemoPhysicalDelegatorNode& other) const;

    MemoPhysicalNodeId getNodeId() const;

private:
    const MemoPhysicalNodeId _nodeId;
};

/**
 * Filter node.
 * It applies a filter over its input.
 *
 * This node is both logical and physical.
 */
class FilterNode final : public Operator<2>, public Node {
    using Base = Operator<2>;

public:
    FilterNode(FilterType filter, ABT child);

    bool operator==(const FilterNode& other) const;

    const FilterType& getFilter() const;
    FilterType& getFilter();

    const ABT& getChild() const;
    ABT& getChild();
};

/**
 * Evaluation node.
 * Adds a new projection to its input.
 *
 * This node is both logical and physical.
 */
class EvaluationNode final : public Operator<2>, public Node {
    using Base = Operator<2>;

public:
    EvaluationNode(ProjectionName projectionName, ProjectionType projection, ABT child);

    bool operator==(const EvaluationNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<1>();
        tassert(6624015, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ProjectionName& getProjectionName() const {
        return binder().names()[0];
    }

    const ProjectionType& getProjection() const {
        return binder().exprs()[0];
    }

    const ABT& getChild() const {
        return get<0>();
    }

    ABT& getChild() {
        return get<0>();
    }
};

/**
 * RID intersection node.
 * This is a logical node representing either index-index intersection or index-collection scan
 * (seek) fetch.
 *
 * It is equivalent to a join node with the difference that RID projections do not exist on logical
 * level, and thus projection names are not determined until physical optimization. We want to also
 * restrict the type of operations on RIDs (in this case only set intersection) as opposed to say
 * filter on rid = 5.
 */
class RIDIntersectNode final : public Operator<2>, public ExclusivelyLogicalNode {
    using Base = Operator<2>;

public:
    RIDIntersectNode(ProjectionName scanProjectionName,
                     bool hasLeftIntervals,
                     bool hasRightIntervals,
                     ABT leftChild,
                     ABT rightChild);

    bool operator==(const RIDIntersectNode& other) const;

    const ABT& getLeftChild() const;
    ABT& getLeftChild();

    const ABT& getRightChild() const;
    ABT& getRightChild();

    const ProjectionName& getScanProjectionName() const;

    bool hasLeftIntervals() const;
    bool hasRightIntervals() const;

private:
    const ProjectionName _scanProjectionName;

    // If true left and right children have at least one proper interval (not fully open).
    const bool _hasLeftIntervals;
    const bool _hasRightIntervals;
};

/**
 * Sargable node.
 * This is a logical node which represents special kinds of (simple) evaluations and filters which
 * are amenable to being used in indexing or covered scans.
 *
 * It collects a conjunction of predicates in the following form:
 *    <path, inputProjection> -> <interval, outputProjection>

 * For example to encode a conjunction which encodes filtering with array traversal on "a"
 ($match(a: {$gt, 1}} combined with a retrieval of the field "b" (without restrictions on its
 value).
 *      PathGet "a" Traverse Id | scan_0     ->  [1, +inf], <none>
 *      PathGet "b" Id          | scan_0      -> (-inf, +inf),  "pb"
 */
class SargableNode final : public Operator<3>, public ExclusivelyLogicalNode {
    using Base = Operator<3>;

public:
    /**
     * Maximum size of the PartialSchemaRequirements that can be used to create a SargableNode. We
     * use a 64-bit mask when splitting into left and right requirements.
     */
    static constexpr size_t kMaxPartialSchemaRequirements = 64;

    SargableNode(PartialSchemaRequirements reqMap,
                 CandidateIndexes candidateIndexes,
                 boost::optional<ScanParams> scanParams,
                 IndexReqTarget target,
                 ABT child);

    bool operator==(const SargableNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<1>();
        tassert(6624016, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ABT& getChild() const {
        return get<0>();
    }
    ABT& getChild() {
        return get<0>();
    }

    const PartialSchemaRequirements& getReqMap() const;
    const CandidateIndexes& getCandidateIndexes() const;
    const boost::optional<ScanParams>& getScanParams() const;

    IndexReqTarget getTarget() const;

private:
    const PartialSchemaRequirements _reqMap;

    CandidateIndexes _candidateIndexes;
    boost::optional<ScanParams> _scanParams;

    // Performance optimization to limit number of groups.
    // Under what indexing requirements can this node be implemented.
    const IndexReqTarget _target;
};

#define JOIN_TYPE(F) \
    F(Inner)         \
    F(Left)          \
    F(Right)         \
    F(Full)

MAKE_PRINTABLE_ENUM(JoinType, JOIN_TYPE);
MAKE_PRINTABLE_ENUM_STRING_ARRAY(JoinTypeEnum, JoinType, JOIN_TYPE);
#undef JOIN_TYPE

/**
 * Logical binary join.
 * Join of two logical nodes. Can express inner and outer joins, with an associated join predicate.
 *
 * This node is logical, with a default physical implementation corresponding to a Nested Loops Join
 * (NLJ).
 * Variables used in the inner (right) side are automatically bound with variables from the left
 * (outer) side.
 */
class BinaryJoinNode final : public Operator<3>, public Node {
    using Base = Operator<3>;

public:
    BinaryJoinNode(JoinType joinType,
                   ProjectionNameSet correlatedProjectionNames,
                   FilterType filter,
                   ABT leftChild,
                   ABT rightChild);

    bool operator==(const BinaryJoinNode& other) const;

    JoinType getJoinType() const;

    const ProjectionNameSet& getCorrelatedProjectionNames() const;

    const ABT& getLeftChild() const;
    ABT& getLeftChild();

    const ABT& getRightChild() const;
    ABT& getRightChild();

    const ABT& getFilter() const;

private:
    const JoinType _joinType;

    // Those projections must exist on the outer side and are used to bind free variables on the
    // inner side.
    const ProjectionNameSet _correlatedProjectionNames;
};

/**
 * Physical hash join node.
 * Join condition is a conjunction of pairwise equalities between corresponding left and right keys.
 * It assumes the outer side is probe side and inner side is "build" side. Currently supports only
 * inner joins.
 */
class HashJoinNode final : public Operator<3>, public ExclusivelyPhysicalNode {
    using Base = Operator<3>;

public:
    HashJoinNode(JoinType joinType,
                 ProjectionNameVector leftKeys,
                 ProjectionNameVector rightKeys,
                 ABT leftChild,
                 ABT rightChild);

    bool operator==(const HashJoinNode& other) const;

    JoinType getJoinType() const;
    const ProjectionNameVector& getLeftKeys() const;
    const ProjectionNameVector& getRightKeys() const;

    const ABT& getLeftChild() const;
    ABT& getLeftChild();

    const ABT& getRightChild() const;
    ABT& getRightChild();

private:
    const JoinType _joinType;

    // Join condition is a conjunction of _leftKeys.at(i) == _rightKeys.at(i).
    const ProjectionNameVector _leftKeys;
    const ProjectionNameVector _rightKeys;
};

/**
 * Merge Join node.
 * This is a physical node representing joining of two sorted inputs.
 */
class MergeJoinNode final : public Operator<3>, public ExclusivelyPhysicalNode {
    using Base = Operator<3>;

public:
    MergeJoinNode(ProjectionNameVector leftKeys,
                  ProjectionNameVector rightKeys,
                  std::vector<CollationOp> collation,
                  ABT leftChild,
                  ABT rightChild);

    bool operator==(const MergeJoinNode& other) const;

    const ProjectionNameVector& getLeftKeys() const;
    const ProjectionNameVector& getRightKeys() const;

    const std::vector<CollationOp>& getCollation() const;

    const ABT& getLeftChild() const;
    ABT& getLeftChild();

    const ABT& getRightChild() const;
    ABT& getRightChild();

private:
    // Describes how to merge the sorted streams.
    std::vector<CollationOp> _collation;

    // Join condition is a conjunction of _leftKeys.at(i) == _rightKeys.at(i).
    const ProjectionNameVector _leftKeys;
    const ProjectionNameVector _rightKeys;
};

/**
 * Union of several logical nodes. Projections in common to all nodes are logically union-ed in the
 * output. It can be used with a single child just to restrict projections.
 *
 * This node is both logical and physical.
 */
class UnionNode final : public OperatorDynamic<2>, public Node {
    using Base = OperatorDynamic<2>;

public:
    UnionNode(ProjectionNameVector unionProjectionNames, ABTVector children);

    bool operator==(const UnionNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<0>();
        tassert(6624017, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

private:
    // This struct is a workaround to avoid a use-after-move problem while initializing the base
    // class and passing constructor arguments. Due to the way how the base class is designed, we
    // need to std::move the children vector as the first argument to the Base ctor, but then
    // obtain the size of the moved vector while computing the last argument. So, we'll preserve
    // the children's vector size in this struct to avoid this situation.
    struct UnionNodeChildren {
        UnionNodeChildren(ABTVector children) : _nodes(std::move(children)) {
            _numOfNodes = _nodes.size();
        }

        ABTVector _nodes;
        size_t _numOfNodes;
    };

    UnionNode(ProjectionNameVector unionProjectionNames, UnionNodeChildren children);
};

#define GROUPNODETYPE_OPNAMES(F) \
    F(Complete)                  \
    F(Local)                     \
    F(Global)

MAKE_PRINTABLE_ENUM(GroupNodeType, GROUPNODETYPE_OPNAMES);
MAKE_PRINTABLE_ENUM_STRING_ARRAY(GroupNodeTypeEnum, GroupNodeType, GROUPNODETYPE_OPNAMES);
#undef PATHSYNTAX_OPNAMES

/**
 * Group-by node.
 * This node is logical with a default physical implementation corresponding to a hash group-by.
 * Projects the group-by column from its child, and adds aggregation expressions.
 */
class GroupByNode : public Operator<5>, public Node {
    using Base = Operator<5>;

public:
    GroupByNode(ProjectionNameVector groupByProjectionNames,
                ProjectionNameVector aggregationProjectionNames,
                ABTVector aggregationExpressions,
                ABT child);

    GroupByNode(ProjectionNameVector groupByProjectionNames,
                ProjectionNameVector aggregationProjectionNames,
                ABTVector aggregationExpressions,
                GroupNodeType type,
                ABT child);

    bool operator==(const GroupByNode& other) const;

    const ExpressionBinder& binderAgg() const {
        const ABT& result = get<1>();
        tassert(6624018, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ExpressionBinder& binderGb() const {
        const ABT& result = get<3>();
        tassert(6624019, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ProjectionNameVector& getGroupByProjectionNames() const {
        return binderGb().names();
    }

    const ProjectionNameVector& getAggregationProjectionNames() const {
        return binderAgg().names();
    }

    const auto& getAggregationProjections() const {
        return binderAgg().exprs();
    }

    const auto& getGroupByProjections() const {
        return binderGb().exprs();
    }

    const ABTVector& getAggregationExpressions() const;

    const ABT& getChild() const;
    ABT& getChild();

    GroupNodeType getType() const;

private:
    // Used for local-global rewrite.
    GroupNodeType _type;
};

/**
 * Unwind node.
 * Unwinds an embedded relation inside an array. Generates unwinding positions in the CID
 * projection.
 *
 * This node is both logical and physical.
 */
class UnwindNode final : public Operator<3>, public Node {
    using Base = Operator<3>;

public:
    UnwindNode(ProjectionName projectionName,
               ProjectionName pidProjectionName,
               bool retainNonArrays,
               ABT child);

    bool operator==(const UnwindNode& other) const;

    const ExpressionBinder& binder() const {
        const ABT& result = get<1>();
        tassert(6624020, "Invalid binder type", result.is<ExpressionBinder>());
        return *result.cast<ExpressionBinder>();
    }

    const ProjectionName& getProjectionName() const {
        return binder().names()[0];
    }

    const ProjectionName& getPIDProjectionName() const {
        return binder().names()[1];
    }

    const ProjectionType& getProjection() const {
        return binder().exprs()[0];
    }

    const ProjectionType& getPIDProjection() const {
        return binder().exprs()[1];
    }

    const ABT& getChild() const;

    ABT& getChild();

    bool getRetainNonArrays() const;

private:
    const bool _retainNonArrays;
};

/**
 * Unique node.
 *
 * This is a physical node. It encodes an operation which will duplicate the child input using a
 * sequence of given projection names. It is similar to GroupBy using the given projections as a
 * compound grouping key.
 */
class UniqueNode final : public Operator<2>, public ExclusivelyPhysicalNode {
    using Base = Operator<2>;

public:
    UniqueNode(ProjectionNameVector projections, ABT child);

    bool operator==(const UniqueNode& other) const;

    const ProjectionNameVector& getProjections() const;

    const ABT& getChild() const;

private:
    ProjectionNameVector _projections;
};

/**
 * Collation node.
 * This node is both logical and physical.
 *
 * It represents an operator to collate (sort, or cluster) the input.
 */
class CollationNode final : public Operator<2>, public Node {
    using Base = Operator<2>;

public:
    CollationNode(properties::CollationRequirement property, ABT child);

    bool operator==(const CollationNode& other) const;

    const properties::CollationRequirement& getProperty() const;
    properties::CollationRequirement& getProperty();

    const ABT& getChild() const;

    ABT& getChild();

private:
    properties::CollationRequirement _property;
};

/**
 * Limit and skip node.
 * This node is both logical and physical.
 *
 * It limits the size of the input by a fixed amount.
 */
class LimitSkipNode final : public Operator<1>, public Node {
    using Base = Operator<1>;

public:
    LimitSkipNode(properties::LimitSkipRequirement property, ABT child);

    bool operator==(const LimitSkipNode& other) const;

    const properties::LimitSkipRequirement& getProperty() const;
    properties::LimitSkipRequirement& getProperty();

    const ABT& getChild() const;

    ABT& getChild();

private:
    properties::LimitSkipRequirement _property;
};

/**
 * Exchange node.
 * It specifies how the relation is spread across machines in the execution environment.
 * Currently only single-node, and hash-based partitioning are supported.
 *
 * This node is both logical and physical.
 */
class ExchangeNode final : public Operator<2>, public Node {
    using Base = Operator<2>;

public:
    ExchangeNode(properties::DistributionRequirement distribution, ABT child);

    bool operator==(const ExchangeNode& other) const;

    const properties::DistributionRequirement& getProperty() const;
    properties::DistributionRequirement& getProperty();

    const ABT& getChild() const;

    ABT& getChild();

private:
    properties::DistributionRequirement _distribution;

    /**
     * Defined for hash and range-based partitioning.
     */
    const ProjectionName _projectionName;
};

/**
 * Root of the tree that holds references to the output of the query. In the mql case the query
 * outputs a single "column" (aka document) but in a general case (SQL) we can output arbitrary many
 * "columns". We need the internal references for the output projections in order to keep them live,
 * otherwise they would be dropped from the tree by DCE.
 *
 * This node is only logical.
 */
class RootNode final : public Operator<2>, public Node {
    using Base = Operator<2>;

public:
    RootNode(properties::ProjectionRequirement property, ABT child);

    bool operator==(const RootNode& other) const;

    const properties::ProjectionRequirement& getProperty() const;

    const ABT& getChild() const;
    ABT& getChild();

private:
    const properties::ProjectionRequirement _property;
};

}  // namespace mongo::optimizer
