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

template <class T, class T1 = std::conditional_t<std::is_arithmetic_v<T>, const T, const T&>>
inline size_t computeVectorHash(const std::vector<T>& v) {
    size_t result = 17;
    for (T1 e : v) {
        updateHash(result, std::hash<T>()(e));
    }
    return result;
}

template <int typeCode, typename... Args>
inline size_t computeHashSeq(const Args&... seq) {
    size_t result = 17 + typeCode;
    (updateHash(result, seq), ...);
    return result;
}

std::vector<ABT::reference_type> collectComposed(const ABT& n);

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
 * Used to vend out fresh ids for projection names.
 */
class PrefixId {
public:
    std::string getNextId(const std::string& key);

private:
    opt::unordered_map<std::string, int> _idCounterPerKey;
};

ProjectionNameOrderedSet convertToOrderedSet(ProjectionNameSet unordered);

void combineLimitSkipProperties(properties::LimitSkipRequirement& aboveProp,
                                const properties::LimitSkipRequirement& belowProp);

/**
 * Used to track references originating from a set of physical properties.
 */
ProjectionNameSet extractReferencedColumns(const properties::PhysProps& properties);

/**
 * Returns true if all components of the compound interval are equalities.
 */
bool areCompoundIntervalsEqualities(const CompoundIntervalRequirement& intervals);

struct CollationSplitResult {
    bool _validSplit = false;
    ProjectionCollationSpec _leftCollation;
    ProjectionCollationSpec _rightCollation;
};

/**
 * Split a collation requirement between an outer (left) and inner (right) side. The outer side must
 * be a prefix in the collation spec, and the right side a suffix.
 */
CollationSplitResult splitCollationSpec(const ProjectionName& ridProjName,
                                        const ProjectionCollationSpec& collationSpec,
                                        const ProjectionNameSet& leftProjections,
                                        const ProjectionNameSet& rightProjections);

/**
 * Used to extract variable references from a node.
 */
using VariableNameSetType = opt::unordered_set<std::string>;
VariableNameSetType collectVariableReferences(const ABT& n);

/**
 * Appends a path to another path. Performs the append at PathIdentity elements.
 */
class PathAppender {
public:
    PathAppender(ABT toAppend) : _toAppend(std::move(toAppend)) {}

    void transport(ABT& n, const PathIdentity& node) {
        n = _toAppend;
    }

    template <typename T, typename... Ts>
    void transport(ABT& /*n*/, const T& /*node*/, Ts&&...) {
        // noop
    }

    void append(ABT& path) {
        return algebra::transport<true>(path, *this);
    }

private:
    ABT _toAppend;
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

/**
 * Takes an expression that comes from an Filter or Evaluation node, and attempt to convert
 * to a PartialSchemaReqConversion. This is done independent of the availability of indexes.
 * Essentially this means to extract intervals over paths whenever possible. If the conversion is
 * not possible, return empty result.
 */
boost::optional<PartialSchemaReqConversion> convertExprToPartialSchemaReq(const ABT& expr,
                                                                          bool isFilterContext);

/**
 * Given a set of non-multikey paths, remove redundant Traverse elements from paths in a Partial
 * Schema Requirement structure. Returns true if we have an empty result after simplification.
 */
bool simplifyPartialSchemaReqPaths(const ProjectionName& scanProjName,
                                   const IndexPathSet& nonMultiKeyPaths,
                                   PartialSchemaRequirements& reqMap);

/**
 * Check if a path contains a Traverse element.
 */
bool checkPathContainsTraverse(const ABT& path);

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
                                         bool fastNullHandling,
                                         bool& hasEmptyInterval);

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
bool checkMaybeHasNull(const IntervalReqExpr::Node& intervals);

/**
 * Used to lower a Sargable node to a subtree consisting of functionally equivalent Filter and Eval
 * nodes.
 */
void lowerPartialSchemaRequirement(const PartialSchemaKey& key,
                                   const PartialSchemaRequirement& req,
                                   ABT& node,
                                   const std::function<void(const ABT& node)>& visitor =
                                       [](const ABT&) {});

void lowerPartialSchemaRequirements(CEType scanGroupCE,
                                    std::vector<SelectivityType> indexPredSels,
                                    ResidualRequirementsWithCE& requirements,
                                    ABT& physNode,
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

ABT lowerIntervals(PrefixId& prefixId,
                   const ProjectionName& ridProjName,
                   FieldProjectionMap indexProjectionMap,
                   const std::string& scanDefName,
                   const std::string& indexDefName,
                   const CompoundIntervalReqExpr::Node& intervals,
                   bool reverseOrder,
                   CEType indexCE,
                   CEType scanGroupCE,
                   NodeCEMap& nodeCEMap);


}  // namespace mongo::optimizer
