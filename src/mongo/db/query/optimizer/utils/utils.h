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

namespace mongo::optimizer {

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
 * Returns a vector all paths nested under conjunctions (PathComposeM) in the given path.
 * For example, PathComposeM(PathComposeM(Foo, Bar), Baz) returns [Foo, Bar, Baz].
 * If the given path is not a conjunction, returns a vector with the given path.
 */
std::vector<ABT::reference_type> collectComposed(const ABT& n);

/**
 * Like collectComposed() but bounded by a maximum number of composed paths.
 * If the given path has more PathComposeM;s than specified by maxDepth, then return a vector
 * with the given path. Otherwise, returns the result of collectComposed().
 *
 * This is useful for preventing the optimizer from unintentionally creating a very deep tree which
 * causes stack-overflow on a recursive traversal.
 */
std::vector<ABT::reference_type> collectComposedBounded(const ABT& n, size_t maxDepth);

/**
 * Returns true if the path represented by 'node' is of the form PathGet "field" PathId
 */
bool isSimplePath(const ABT& node);

template <class Element = PathComposeM>
inline void maybeComposePath(ABT& composition, ABT child) {
    if (child.is<PathIdentity>()) {
        return;
    }
    if (composition.is<PathIdentity>()) {
        composition = std::move(child);
        return;
    }

    composition = make<Element>(std::move(composition), std::move(child));
}

/**
 * Creates a balanced tree of composition elements over the input vector which it modifies in place.
 * In the end at most one element remains in the vector.
 */
template <class Element = PathComposeM>
inline void maybeComposePaths(ABTVector& paths) {
    while (paths.size() > 1) {
        const size_t half = paths.size() / 2;
        for (size_t i = 0; i < half; i++) {
            maybeComposePath<Element>(paths.at(i), std::move(paths.back()));
            paths.pop_back();
        }
    }
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
 * Used to access and manipulate the first child of a n-ary node.
 */
template <class NodeType>
struct FirstChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->nodes().front();
    }

    ABT& operator()(ABT& node) {
        return node.cast<NodeType>()->nodes().front();
    }
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

/**
 * Used to vend out fresh spool ids.
 */
class SpoolId {
public:
    SpoolId() : _nextId(0) {}

    int64_t getNextId() {
        return _nextId++;
    }

private:
    int64_t _nextId;
};

ProjectionNameOrderedSet convertToOrderedSet(ProjectionNameSet unordered);

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
void restrictProjections(ProjectionNameVector projNames, ABT& input);

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

/**
 * Appends a path to another path. Performs the append at PathIdentity elements.
 */
class PathAppender {
public:
    PathAppender(ABT suffix) : _suffix(std::move(suffix)) {}

    void transport(ABT& n, const PathIdentity& node) {
        n = _suffix;
    }

    template <typename T, typename... Ts>
    void transport(ABT& /*n*/, const T& /*node*/, Ts&&...) {
        // noop
    }

    /**
     * Concatenate 'prefix' and 'suffix' by modifying 'prefix' in place.
     */
    static void appendInPlace(ABT& prefix, ABT suffix) {
        PathAppender instance{std::move(suffix)};
        algebra::transport<true>(prefix, instance);
    }

    /**
     * Return the concatenation of 'prefix' and 'suffix'.
     */
    [[nodiscard]] static ABT append(ABT prefix, ABT suffix) {
        appendInPlace(prefix, std::move(suffix));
        return prefix;
    }

private:
    ABT _suffix;
};

struct PartialSchemaReqConversion {
    PartialSchemaReqConversion(PartialSchemaRequirements reqMap);
    PartialSchemaReqConversion(ABT bound);

    // If set, contains a Constant or Variable bound of an (yet unknown) interval.
    boost::optional<ABT> _bound;

    // Requirements we have built so far.
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
 * Given a path and a MultikeynessTrie describing the path's input,
 * removes any Traverse nodes that we know will never encounter an array.
 *
 * Returns true if any changes were made to the ABT.
 */
bool simplifyTraverseNonArray(ABT& path, const MultikeynessTrie& multikeynessTrie);

/**
 * Given a set of non-multikey paths, remove redundant Traverse elements from paths in a Partial
 * Schema Requirement structure. Returns true if we have an empty result after simplification.
 */
bool simplifyPartialSchemaReqPaths(const ProjectionName& scanProjName,
                                   const MultikeynessTrie& multikeynessTrie,
                                   PartialSchemaRequirements& reqMap,
                                   const ConstFoldFn& constFold);

/**
 * Check if a path contains a Traverse element.
 */
bool checkPathContainsTraverse(const ABT& path);

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
 *   bindings that would be redundant (have the same key). Each
 *   redundant binding gets an entry in 'projectionRenames', which maps
 *   the redundant name to the de-duplicated name.
 *
 * "Failure" means we are unable to represent the result as a PartialSchemaRequirements.
 * This can happen when:
 * - The resulting predicate is always false.
 * - 'source' reads from a projection bound by 'target'.
 */
bool intersectPartialSchemaReq(PartialSchemaRequirements& target,
                               const PartialSchemaRequirements& source,
                               ProjectionRenames& projectionRenames);


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
 * If the intersection of any of the interval requirements in 'reqMap' results in an empty
 * interval, return an empty mapping and set 'hasEmptyInterval' to true.
 * Otherwise return the computed mapping, and set 'hasEmptyInterval' to false.
 */
CandidateIndexes computeCandidateIndexes(PrefixId& prefixId,
                                         const ProjectionName& scanProjectionName,
                                         const PartialSchemaRequirements& reqMap,
                                         const ScanDefinition& scanDef,
                                         const QueryHints& hints,
                                         bool& hasEmptyInterval,
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
                                   ABT& node,
                                   const PathToIntervalFn& pathToInterval,
                                   const std::function<void(const ABT& node)>& visitor =
                                       [](const ABT&) {});

void lowerPartialSchemaRequirements(CEType scanGroupCE,
                                    std::vector<SelectivityType> indexPredSels,
                                    ResidualRequirementsWithCE& requirements,
                                    ABT& physNode,
                                    const PathToIntervalFn& pathToInterval,
                                    NodeCEMap& nodeCEMap);

void sortResidualRequirements(ResidualRequirementsWithCE& residualReq);

void applyProjectionRenames(ProjectionRenames projectionRenames,
                            ABT& node,
                            const std::function<void(const ABT& node)>& visitor = [](const ABT&) {
                            });

void removeRedundantResidualPredicates(const ProjectionNameOrderPreservingSet& requiredProjections,
                                       ResidualRequirements& residualReqs,
                                       FieldProjectionMap& fieldProjectionMap);

/**
 * Implements an RID Intersect node using Union and GroupBy.
 */
ABT lowerRIDIntersectGroupBy(PrefixId& prefixId,
                             const ProjectionName& ridProjName,
                             CEType intersectedCE,
                             CEType leftCE,
                             CEType rightCE,
                             const properties::PhysProps& physProps,
                             const properties::PhysProps& leftPhysProps,
                             const properties::PhysProps& rightPhysProps,
                             ABT leftChild,
                             ABT rightChild,
                             NodeCEMap& nodeCEMap,
                             ChildPropsType& childProps);

/**
 * Implements an RID Intersect node using a HashJoin.
 */
ABT lowerRIDIntersectHashJoin(PrefixId& prefixId,
                              const ProjectionName& ridProjName,
                              CEType intersectedCE,
                              CEType leftCE,
                              CEType rightCE,
                              const properties::PhysProps& leftPhysProps,
                              const properties::PhysProps& rightPhysProps,
                              ABT leftChild,
                              ABT rightChild,
                              NodeCEMap& nodeCEMap,
                              ChildPropsType& childProps);

ABT lowerRIDIntersectMergeJoin(PrefixId& prefixId,
                               const ProjectionName& ridProjName,
                               CEType intersectedCE,
                               CEType leftCE,
                               CEType rightCE,
                               const properties::PhysProps& leftPhysProps,
                               const properties::PhysProps& rightPhysProps,
                               ABT leftChild,
                               ABT rightChild,
                               NodeCEMap& nodeCEMap,
                               ChildPropsType& childProps);

/**
 * Lowers a plan consisting of one or several equality prefixes. The sub-plans for each equality
 * prefix are connected using correlated joins. The sub-plans for each prefix in turn are
 * implemented as one or more index scans which are unioned or intersected depending on the shape of
 * the interval expression (e.g. conjunction or disjunction).
 */
ABT lowerEqPrefixes(PrefixId& prefixId,
                    const ProjectionName& ridProjName,
                    FieldProjectionMap indexProjectionMap,
                    const std::string& scanDefName,
                    const std::string& indexDefName,
                    SpoolId& spoolId,
                    size_t indexFieldCount,
                    const std::vector<EqualityPrefixEntry>& eqPrefixes,
                    size_t eqPrefixIndex,
                    const std::vector<bool>& reverseOrder,
                    ProjectionNameVector correlatedProjNames,
                    const std::map<size_t, SelectivityType>& indexPredSelMap,
                    CEType indexCE,
                    CEType scanGroupCE,
                    NodeCEMap& nodeCEMap);

/**
 * This helper checks to see if we have a PathTraverse + PathId at the end of the path.
 */
bool pathEndsInTraverse(const optimizer::ABT& path);

bool hasProperIntervals(const PartialSchemaRequirements& reqMap);
}  // namespace mongo::optimizer
