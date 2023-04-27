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

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/utils/physical_plan_builder.h"
#include "mongo/util/id_generator.h"


namespace mongo::optimizer {
template <typename Builder>
ABT makeBalancedTreeImpl(Builder builder, std::vector<ABT>& leaves, size_t from, size_t until) {
    invariant(from < until);
    if (from + 1 == until) {
        return std::move(leaves[from]);
    } else {
        size_t mid = from + (until - from) / 2;
        auto lhs = makeBalancedTreeImpl(builder, leaves, from, mid);
        auto rhs = makeBalancedTreeImpl(builder, leaves, mid, until);
        return builder(std::move(lhs), std::move(rhs));
    }
}

template <typename Builder>
ABT makeBalancedTree(Builder builder, std::vector<ABT> leaves) {
    return makeBalancedTreeImpl(builder, leaves, 0, leaves.size());
}

ABT makeBalancedBooleanOpTree(Operations logicOp, std::vector<ABT> leaves);

inline void updateHash(size_t& result, const size_t hash) {
    result = 31 * result + hash;
}

inline void updateHashUnordered(size_t& result, const size_t hash) {
    result ^= hash;
}

template <class T,
          class Hasher = std::hash<T>,
          class T1 = std::conditional_t<std::is_arithmetic_v<T>, const T, const T&>>
inline size_t computeVectorHash(const std::vector<T>& v) {
    size_t result = 17;
    for (T1 e : v) {
        updateHash(result, Hasher()(e));
    }
    return result;
}

template <int typeCode, typename... Args>
inline size_t computeHashSeq(const Args&... seq) {
    size_t result = 17 + typeCode;
    (updateHash(result, seq), ...);
    return result;
}

/**
 * Used to access and manipulate the child of a unary node.
 */
template <class NodeType>
struct DefaultChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->getChild();
    }

    ABT& operator()(ABT& node) const {
        return node.cast<NodeType>()->getChild();
    }
};

/**
 * Used to access and manipulate the left child of a binary node.
 */
template <class NodeType>
struct LeftChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->getLeftChild();
    }

    ABT& operator()(ABT& node) const {
        return node.cast<NodeType>()->getLeftChild();
    }
};

/**
 * Used to access and manipulate the right child of a binary node.
 */
template <class NodeType>
struct RightChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->getRightChild();
    }

    ABT& operator()(ABT& node) const {
        return node.cast<NodeType>()->getRightChild();
    }
};

/**
 * Used to access children of a n-ary node. By default, it accesses the first child.
 */
template <class NodeType>
struct IndexedChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->nodes().at(index);
    }

    ABT& operator()(ABT& node) {
        return node.cast<NodeType>()->nodes().at(index);
    }

    size_t index = 0;
};

/**
 * Used to vend out fresh projection names. The method getNextId receives an optional prefix. If we
 * are generating descriptive names, the variable name we return starts with the prefix and includes
 * a prefix-specific counter. If we are not generating descriptive variable names, the prefix is
 * ignored and instead we use a global counter instead and ignore the prefix.
 */
class PrefixId {
    using IdType = uint64_t;
    using PrefixMapType = opt::unordered_map<std::string, IdType>;

public:
    static PrefixId create(const bool useDescriptiveVarNames) {
        return {useDescriptiveVarNames};
    }
    static PrefixId createForTests() {
        return {true /*useDescripriveVarNames*/};
    }

    template <size_t N>
    ProjectionName getNextId(const char (&prefix)[N]) {
        if (std::holds_alternative<IdType>(_ids)) {
            return ProjectionName{str::stream() << "p" << std::get<IdType>(_ids)++};
        } else {
            return ProjectionName{str::stream()
                                  << prefix << "_" << std::get<PrefixMapType>(_ids)[prefix]++};
        }
    }

    PrefixId(const PrefixId& other) = delete;
    PrefixId(PrefixId&& other) = default;

    PrefixId& operator=(const PrefixId& other) = delete;
    PrefixId& operator=(PrefixId&& other) = default;

private:
    PrefixId(const bool useDescriptiveVarNames) {
        if (useDescriptiveVarNames) {
            _ids = {PrefixMapType{}};
        } else {
            _ids = {uint64_t{}};
        }
    }

    std::variant<IdType, PrefixMapType> _ids;
};

using SpoolIdGenerator = IdGenerator<int64_t>;

void combineLimitSkipProperties(properties::LimitSkipRequirement& aboveProp,
                                const properties::LimitSkipRequirement& belowProp);

properties::LogicalProps createInitialScanProps(const ProjectionName& projectionName,
                                                const std::string& scanDefName,
                                                GroupIdType groupId = -1,
                                                properties::DistributionSet distributions = {});

/**
 * Used to track references originating from a set of physical properties.
 */
ProjectionNameSet extractReferencedColumns(const properties::PhysProps& properties);

// Use a union node to restrict the set of projections we expose up the tree. The union node is
// optimized away during lowering.
void restrictProjections(ProjectionNameVector projNames, CEType ce, PhysPlanBuilder& input);

struct CollationSplitResult {
    bool _validSplit = false;
    ProjectionCollationSpec _leftCollation;
    ProjectionCollationSpec _rightCollation;
};

/**
 * Split a collation requirement between an outer (left) and inner (right) side. The outer side must
 * be a prefix in the collation spec, and the right side a suffix.
 */
CollationSplitResult splitCollationSpec(const boost::optional<ProjectionName>& ridProjName,
                                        const ProjectionCollationSpec& collationSpec,
                                        const ProjectionNameSet& leftProjections,
                                        const ProjectionNameSet& rightProjections);

struct PartialSchemaReqConversion {
    PartialSchemaReqConversion(PartialSchemaRequirements reqMap);
    PartialSchemaReqConversion(ABT bound);

    // If set, contains a Constant or Variable bound of an (yet unknown) interval.
    boost::optional<ABT> _bound;

    // Requirements we have built so far. May be trivially true.
    PartialSchemaRequirements _reqMap;

    // Have we added a PathComposeM.
    bool _hasIntersected;

    // Have we added a PathTraverse.
    bool _hasTraversed;

    // If true, retain original predicate after the conversion. In this case, the requirement map
    // might capture only a part of the predicate.
    // TODO: consider generalizing to retain only a part of the predicate.
    bool _retainPredicate;
};

using PathToIntervalFn = std::function<boost::optional<IntervalReqExpr::Node>(const ABT&)>;

/**
 * Takes an expression that comes from an Filter or Evaluation node, and attempt to convert
 * to a PartialSchemaReqConversion. This is done independent of the availability of indexes.
 * Essentially this means to extract intervals over paths whenever possible. If the conversion is
 * not possible, return empty result.
 *
 * A direct node-to-intervals converter may be specified, used to selectively expands for example
 * PathArr into an equivalent interval representation.
 */
boost::optional<PartialSchemaReqConversion> convertExprToPartialSchemaReq(
    const ABT& expr, bool isFilterContext, const PathToIntervalFn& pathToInterval);

/**
 * Given a set of non-multikey paths, remove redundant Traverse elements from paths in a Partial
 * Schema Requirement structure. Following that the intervals of any remaining non-multikey paths
 * (following simplification) on the same key are intersected. Intervals of multikey paths are
 * checked for subsumption and if one subsumes the other, the subsuming one is retained. Returns
 * true if we have an always-false predicate after simplification. Each redundant binding gets an
 * entry in 'projectionRenames', which maps redundant name to the de-duplicated name.
 */
[[nodiscard]] bool simplifyPartialSchemaReqPaths(
    const boost::optional<ProjectionName>& scanProjName,
    const MultikeynessTrie& multikeynessTrie,
    PartialSchemaRequirements& reqMap,
    ProjectionRenames& projectionRenames,
    const ConstFoldFn& constFold);

/**
 * Try to check whether the predicate 'lhs' is a subset of 'rhs'.
 *
 * True means 'lhs' is contained in 'rhs': every document that matches
 * 'lhs' also matches 'rhs'.
 *
 * False means either:
 * - Not a subset: there is a counterexample.
 * - Not sure: this function was unable to determine one way or the other.
 */
bool isSubsetOfPartialSchemaReq(const PartialSchemaRequirements& lhs,
                                const PartialSchemaRequirements& rhs);

/**
 * Computes the intersection of two PartialSchemeRequirements objects.
 * On success, returns true and stores the result in 'target'.
 * On failure, returns false and leaves 'target' in an unspecified state.
 *
 * Assumes 'target' comes before 'source', so 'source' may refer to bindings
 * produced by 'target'.
 *
 * The intersection:
 * - is a predicate that matches iff both original predicates match.
 * - has all the bindings from 'target' and 'source', but excluding
 *   bindings that would be redundant (have the same key).
 *
 * "Failure" means we are unable to represent the result as a PartialSchemaRequirements.
 * This can happen when:
 * - The resulting predicate is always false.
 * - 'source' reads from a projection bound by 'target'.
 */
bool intersectPartialSchemaReq(PartialSchemaRequirements& target,
                               const PartialSchemaRequirements& source);

PartialSchemaRequirements unionPartialSchemaReq(PartialSchemaRequirements&& left,
                                                PartialSchemaRequirements&& right);


/**
 * Encode an index of an index field as a field name in order to use with a FieldProjectionMap.
 */
std::string encodeIndexKeyName(size_t indexField);

/**
 * Decode an field name as an index field.
 */
size_t decodeIndexKeyName(const std::string& fieldName);

/**
 * Compute a list of candidate indexes. A CandidateIndexEntry describes intervals that could be
 * used for accessing each of the indexes in the map. The intervals themselves are derived from
 * 'reqMap'.
 */
CandidateIndexes computeCandidateIndexes(PrefixId& prefixId,
                                         const ProjectionName& scanProjectionName,
                                         const PartialSchemaRequirements& reqMap,
                                         const ScanDefinition& scanDef,
                                         const QueryHints& hints,
                                         const ConstFoldFn& constFold);

/**
 * Computes a set of residual predicates which will be applied on top of a Scan.
 */
boost::optional<ScanParams> computeScanParams(PrefixId& prefixId,
                                              const PartialSchemaRequirements& reqMap,
                                              const ProjectionName& rootProj);

/**
 * Checks if we have an interval tree which has at least one atomic interval which may include Null
 * as an endpoint.
 */
bool checkMaybeHasNull(const IntervalReqExpr::Node& intervals, const ConstFoldFn& constFold);

/**
 * Used to lower a Sargable node to a subtree consisting of functionally equivalent Filter and Eval
 * nodes.
 */
void lowerPartialSchemaRequirement(const PartialSchemaKey& key,
                                   const PartialSchemaRequirement& req,
                                   const PathToIntervalFn& pathToInterval,
                                   boost::optional<CEType> residualCE,
                                   PhysPlanBuilder& builder);

/**
 * Lower ResidualRequirementsWithCE to a subtree consisting of functionally equivalent Filter and
 * Eval nodes. Note that we take indexPredSels by value here because the implementation needs its
 * own copy. "scanGroupCE" is the estimated cardinality of the underlying collection scan (the
 * Sargable node's child group), while the "baseCE" is the initial cardinality on top of which the
 * residual predicates act. For a sargable node with a "Seek" target it is "1" to reflect the fact
 * that we fetch one row id at a time, and for "Complete" and "Index" it is the same as the
 * "scanGroupCE".
 */
void lowerPartialSchemaRequirements(boost::optional<CEType> scanGroupCE,
                                    boost::optional<CEType> baseCE,
                                    std::vector<SelectivityType> indexPredSels,
                                    ResidualRequirementsWithOptionalCE::Node requirements,
                                    const PathToIntervalFn& pathToInterval,
                                    PhysPlanBuilder& builder);

/**
 * Build ResidualRequirementsWithOptionalCE by combining 'residReqs' with the corresponding entries
 * in 'partialSchemaKeyCE'.
 */
ResidualRequirementsWithOptionalCE::Node createResidualReqsWithCE(
    const ResidualRequirements::Node& residReqs, const PartialSchemaKeyCE& partialSchemaKeyCE);

/**
 * Build ResidualRequirementsWithOptionalCE where each entry in 'reqs' has boost::none CE.
 */
ResidualRequirementsWithOptionalCE::Node createResidualReqsWithEmptyCE(const PSRExpr::Node& reqs);

/**
 * Sort requirements under a Conjunction by estimated cost.
 */
void sortResidualRequirements(ResidualRequirementsWithOptionalCE::Node& residualReq);

void applyProjectionRenames(ProjectionRenames projectionRenames, ABT& node);

/**
 * Remove unused requirements from 'residualReqs' and remove unused projections from
 * 'fieldProjectionMap'. A boost::none 'residualReqs' indicates that there are no residual
 * requirements to be applied after the IndexScan, PhysicalScan or Seek (can be thought of as
 * "trivially true" residual requirements).
 */
void removeRedundantResidualPredicates(const ProjectionNameOrderPreservingSet& requiredProjections,
                                       boost::optional<ResidualRequirements::Node>& residualReqs,
                                       FieldProjectionMap& fieldProjectionMap);

/**
 * Implements an RID Intersect node using Union and GroupBy.
 */
PhysPlanBuilder lowerRIDIntersectGroupBy(PrefixId& prefixId,
                                         const ProjectionName& ridProjName,
                                         CEType intersectedCE,
                                         CEType leftCE,
                                         CEType rightCE,
                                         const properties::PhysProps& physProps,
                                         const properties::PhysProps& leftPhysProps,
                                         const properties::PhysProps& rightPhysProps,
                                         PhysPlanBuilder leftChild,
                                         PhysPlanBuilder rightChild,
                                         ChildPropsType& childProps);

/**
 * Implements an RID Intersect node using a HashJoin.
 */
PhysPlanBuilder lowerRIDIntersectHashJoin(PrefixId& prefixId,
                                          const ProjectionName& ridProjName,
                                          CEType intersectedCE,
                                          CEType leftCE,
                                          CEType rightCE,
                                          const properties::PhysProps& leftPhysProps,
                                          const properties::PhysProps& rightPhysProps,
                                          PhysPlanBuilder leftChild,
                                          PhysPlanBuilder rightChild,
                                          ChildPropsType& childProps);

PhysPlanBuilder lowerRIDIntersectMergeJoin(PrefixId& prefixId,
                                           const ProjectionName& ridProjName,
                                           CEType intersectedCE,
                                           CEType leftCE,
                                           CEType rightCE,
                                           const properties::PhysProps& leftPhysProps,
                                           const properties::PhysProps& rightPhysProps,
                                           PhysPlanBuilder leftChild,
                                           PhysPlanBuilder rightChild,
                                           ChildPropsType& childProps);

/**
 * Lowers a plan consisting of one or several equality prefixes. The sub-plans for each equality
 * prefix are connected using correlated joins. The sub-plans for each prefix in turn are
 * implemented as one or more index scans which are unioned or intersected depending on the shape of
 * the interval expression (e.g. conjunction or disjunction).
 */
PhysPlanBuilder lowerEqPrefixes(PrefixId& prefixId,
                                const ProjectionName& ridProjName,
                                FieldProjectionMap indexProjectionMap,
                                const std::string& scanDefName,
                                const std::string& indexDefName,
                                SpoolIdGenerator& spoolId,
                                size_t indexFieldCount,
                                const std::vector<EqualityPrefixEntry>& eqPrefixes,
                                size_t eqPrefixIndex,
                                const std::vector<bool>& reverseOrder,
                                ProjectionNameVector correlatedProjNames,
                                const std::map<size_t, SelectivityType>& indexPredSelMap,
                                CEType indexCE,
                                CEType scanGroupCE,
                                bool useSortedMerge);

bool hasProperIntervals(const PSRExpr::Node& reqs);
}  // namespace mongo::optimizer
