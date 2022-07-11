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

#include "mongo/db/query/optimizer/utils/utils.h"

#include "boost/none.hpp"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

size_t roundUpToNextPow2(const size_t v, const size_t maxPower) {
    if (v == 0) {
        return 0;
    }

    size_t result = 1;
    for (size_t power = 0; result < v && power < maxPower; result <<= 1, power++) {
    }
    return result;
}

std::vector<ABT::reference_type> collectComposed(const ABT& n) {
    if (auto comp = n.cast<PathComposeM>(); comp) {
        auto lhs = collectComposed(comp->getPath1());
        auto rhs = collectComposed(comp->getPath2());
        lhs.insert(lhs.end(), rhs.begin(), rhs.end());

        return lhs;
    }

    return {n.ref()};
}

bool isSimplePath(const ABT& node) {
    if (auto getPtr = node.cast<PathGet>();
        getPtr != nullptr && getPtr->getPath().is<PathIdentity>()) {
        return true;
    }
    return false;
}

std::string PrefixId::getNextId(const std::string& key) {
    std::ostringstream os;
    os << key << "_" << _idCounterPerKey[key]++;
    return os.str();
}

ProjectionNameOrderedSet convertToOrderedSet(ProjectionNameSet unordered) {
    ProjectionNameOrderedSet ordered;
    for (const ProjectionName& projection : unordered) {
        ordered.emplace(projection);
    }
    return ordered;
}

void combineLimitSkipProperties(properties::LimitSkipRequirement& aboveProp,
                                const properties::LimitSkipRequirement& belowProp) {
    using namespace properties;

    const int64_t newAbsLimit = std::min<int64_t>(
        aboveProp.hasLimit() ? (belowProp.getSkip() + aboveProp.getAbsoluteLimit())
                             : LimitSkipRequirement::kMaxVal,
        std::max<int64_t>(0,
                          belowProp.hasLimit()
                              ? (belowProp.getAbsoluteLimit() - aboveProp.getSkip())
                              : LimitSkipRequirement::kMaxVal));

    const int64_t newLimit = (newAbsLimit == LimitSkipRequirement::kMaxVal)
        ? LimitSkipRequirement::kMaxVal
        : (newAbsLimit - belowProp.getSkip());
    const int64_t newSkip = (newLimit == 0) ? 0 : belowProp.getSkip();
    aboveProp = {newLimit, newSkip};
}

/**
 * Used to track references originating from a set of properties.
 */
class PropertiesAffectedColumnsExtractor {
public:
    template <class T>
    void operator()(const properties::PhysProperty&, const T& prop) {
        for (const ProjectionName& projection : prop.getAffectedProjectionNames()) {
            _projections.insert(projection);
        }
    }

    static ProjectionNameSet extract(const properties::PhysProps& properties) {
        PropertiesAffectedColumnsExtractor extractor;
        for (const auto& entry : properties) {
            entry.second.visit(extractor);
        }
        return extractor._projections;
    }

private:
    ProjectionNameSet _projections;
};

ProjectionNameSet extractReferencedColumns(const properties::PhysProps& properties) {
    return PropertiesAffectedColumnsExtractor::extract(properties);
}

bool areMultiKeyIntervalsEqualities(const MultiKeyIntervalRequirement& intervals) {
    for (const auto& interval : intervals) {
        if (!interval.isEquality()) {
            return false;
        }
    }
    return true;
}

CollationSplitResult splitCollationSpec(const ProjectionName& ridProjName,
                                        const ProjectionCollationSpec& collationSpec,
                                        const ProjectionNameSet& leftProjections,
                                        const ProjectionNameSet& rightProjections) {
    bool leftSide = true;
    ProjectionCollationSpec leftCollationSpec;
    ProjectionCollationSpec rightCollationSpec;

    for (size_t index = 0; index < collationSpec.size(); index++) {
        const auto& collationEntry = collationSpec[index];

        const ProjectionName& projectionName = collationEntry.first;
        if (projectionName == ridProjName) {
            uassert(6624147, "Collation on RID must be last", index + 1 == collationSpec.size());

            // Propagate collation requirement on rid only to left side.
            leftCollationSpec.emplace_back(collationEntry);
        } else if (leftProjections.count(projectionName) > 0) {
            if (!leftSide) {
                // Left and right projections must complement and form prefix and suffix.
                return {};
            }
            leftCollationSpec.push_back(collationEntry);
        } else if (rightProjections.count(projectionName) > 0) {
            if (leftSide) {
                leftSide = false;
            }
            rightCollationSpec.push_back(collationEntry);
        } else {
            uasserted(6624146,
                      "Collation projection must appear in either the left or the right "
                      "child projections");
            return {};
        }
    }

    return {true /*validSplit*/, std::move(leftCollationSpec), std::move(rightCollationSpec)};
}
/**
 * Helper class used to extract variable references from a node.
 */
class NodeVariableTracker {
public:
    template <typename T, typename... Ts>
    VariableNameSetType walk(const T&, Ts&&...) {
        static_assert(!std::is_base_of_v<Node, T>, "Nodes must implement variable tracking");

        // Default case: no variables.
        return {};
    }

    VariableNameSetType walk(const ScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    VariableNameSetType walk(const ValueScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    VariableNameSetType walk(const PhysicalScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    VariableNameSetType walk(const CoScanNode& /*node*/) {
        return {};
    }

    VariableNameSetType walk(const IndexScanNode& /*node*/, const ABT& /*binds*/) {
        return {};
    }

    VariableNameSetType walk(const SeekNode& /*node*/, const ABT& /*binds*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const MemoLogicalDelegatorNode& /*node*/) {
        return {};
    }

    VariableNameSetType walk(const MemoPhysicalDelegatorNode& /*node*/) {
        return {};
    }

    VariableNameSetType walk(const FilterNode& /*node*/, const ABT& /*child*/, const ABT& expr) {
        return extractFromABT(expr);
    }

    VariableNameSetType walk(const EvaluationNode& /*node*/,
                             const ABT& /*child*/,
                             const ABT& expr) {
        return extractFromABT(expr);
    }

    VariableNameSetType walk(const SargableNode& /*node*/,
                             const ABT& /*child*/,
                             const ABT& /*binds*/,
                             const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const RIDIntersectNode& /*node*/,
                             const ABT& /*leftChild*/,
                             const ABT& /*rightChild*/) {
        return {};
    }

    VariableNameSetType walk(const BinaryJoinNode& /*node*/,
                             const ABT& /*leftChild*/,
                             const ABT& /*rightChild*/,
                             const ABT& expr) {
        return extractFromABT(expr);
    }

    VariableNameSetType walk(const HashJoinNode& /*node*/,
                             const ABT& /*leftChild*/,
                             const ABT& /*rightChild*/,
                             const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const MergeJoinNode& /*node*/,
                             const ABT& /*leftChild*/,
                             const ABT& /*rightChild*/,
                             const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const UnionNode& /*node*/,
                             const ABTVector& /*children*/,
                             const ABT& /*binder*/,
                             const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const GroupByNode& /*node*/,
                             const ABT& /*child*/,
                             const ABT& /*aggBinder*/,
                             const ABT& aggRefs,
                             const ABT& /*groupbyBinder*/,
                             const ABT& groupbyRefs) {
        VariableNameSetType result;
        extractFromABT(result, aggRefs);
        extractFromABT(result, groupbyRefs);
        return result;
    }

    VariableNameSetType walk(const UnwindNode& /*node*/,
                             const ABT& /*child*/,
                             const ABT& /*binds*/,
                             const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const UniqueNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const CollationNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const LimitSkipNode& /*node*/, const ABT& /*child*/) {
        return {};
    }

    VariableNameSetType walk(const ExchangeNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    VariableNameSetType walk(const RootNode& /*node*/, const ABT& /*child*/, const ABT& refs) {
        return extractFromABT(refs);
    }

    static VariableNameSetType collect(const ABT& n) {
        NodeVariableTracker tracker;
        return algebra::walk<false>(n, tracker);
    }

private:
    void extractFromABT(VariableNameSetType& vars, const ABT& v) {
        const auto& result = VariableEnvironment::getVariables(v);
        for (const Variable& var : result._variables) {
            if (result._definedVars.count(var.name()) == 0) {
                // We are interested in either free variables, or variables defined on other nodes.
                vars.insert(var.name());
            }
        }
    }

    VariableNameSetType extractFromABT(const ABT& v) {
        VariableNameSetType result;
        extractFromABT(result, v);
        return result;
    }
};

VariableNameSetType collectVariableReferences(const ABT& n) {
    return NodeVariableTracker::collect(n);
}

PartialSchemaReqConversion::PartialSchemaReqConversion(PartialSchemaRequirements reqMap)
    : _bound(),
      _reqMap(std::move(reqMap)),
      _hasIntersected(false),
      _hasTraversed(false),
      _hasEmptyInterval(false),
      _retainPredicate(false) {}

PartialSchemaReqConversion::PartialSchemaReqConversion(ABT bound)
    : _bound(std::move(bound)),
      _reqMap(),
      _hasIntersected(false),
      _hasTraversed(false),
      _hasEmptyInterval(false),
      _retainPredicate(false) {}

/**
 * Helper class that builds PartialSchemaRequirements property from an EvalFilter or an EvalPath.
 */
class PartialSchemaReqConverter {
public:
    using ResultType = boost::optional<PartialSchemaReqConversion>;

    PartialSchemaReqConverter(const bool isFilterContext) : _isFilterContext(isFilterContext) {}

    ResultType handleEvalPathAndEvalFilter(ResultType pathResult, ResultType inputResult) {
        if (!pathResult || !inputResult) {
            return {};
        }
        if (pathResult->_bound.has_value() || !inputResult->_bound.has_value() ||
            !inputResult->_reqMap.empty()) {
            return {};
        }

        if (auto boundPtr = inputResult->_bound->cast<Variable>(); boundPtr != nullptr) {
            const ProjectionName& boundVarName = boundPtr->name();
            PartialSchemaRequirements newMap;

            for (auto& [key, req] : pathResult->_reqMap) {
                if (!key._projectionName.empty()) {
                    return {};
                }
                newMap.emplace(PartialSchemaKey{boundVarName, key._path}, std::move(req));
            }

            PartialSchemaReqConversion result{std::move(newMap)};
            result._hasEmptyInterval = pathResult->_hasEmptyInterval;
            result._retainPredicate = pathResult->_retainPredicate;
            return result;
        }

        return {};
    }

    ResultType transport(const ABT& n,
                         const EvalPath& evalPath,
                         ResultType pathResult,
                         ResultType inputResult) {
        return handleEvalPathAndEvalFilter(std::move(pathResult), std::move(inputResult));
    }

    ResultType transport(const ABT& n,
                         const EvalFilter& evalFilter,
                         ResultType pathResult,
                         ResultType inputResult) {
        return handleEvalPathAndEvalFilter(std::move(pathResult), std::move(inputResult));
    }

    static ResultType handleComposition(const bool isMultiplicative,
                                        ResultType leftResult,
                                        ResultType rightResult) {
        if (!leftResult || !rightResult) {
            return {};
        }
        if (leftResult->_bound.has_value() || rightResult->_bound.has_value()) {
            return {};
        }

        auto& leftReqMap = leftResult->_reqMap;
        auto& rightReqMap = rightResult->_reqMap;
        if (isMultiplicative) {
            {
                ProjectionRenames projectionRenames;
                if (!intersectPartialSchemaReq(leftReqMap, rightReqMap, projectionRenames)) {
                    return {};
                }
                if (!projectionRenames.empty()) {
                    return {};
                }
            }

            if (!leftResult->_hasTraversed && !rightResult->_hasTraversed) {
                // Intersect intervals only if we have not traversed. E.g. (-inf, 90) ^ (70, +inf)
                // becomes (70, 90).
                for (auto& [key, req] : leftReqMap) {
                    auto newIntervals = intersectDNFIntervals(req.getIntervals());
                    if (newIntervals) {
                        req.getIntervals() = std::move(newIntervals.get());
                    } else {
                        leftResult->_hasEmptyInterval = true;
                        break;
                    }
                }
            } else if (leftReqMap.size() > 1) {
                // Reject if we have traversed, and composed requirements are more than one.
                return {};
            }

            leftResult->_hasIntersected = true;
            return leftResult;
        }

        // We can only perform additive composition (OR) if we have a single matching key on both
        // sides.
        if (leftReqMap.size() != 1 || rightReqMap.size() != 1) {
            return {};
        }

        auto leftEntry = leftReqMap.begin();
        auto rightEntry = rightReqMap.begin();
        auto& [leftKey, leftReq] = *leftEntry;
        auto& [rightKey, rightReq] = *rightEntry;

        if (leftKey == rightKey) {
            combineIntervalsDNF(false /*intersect*/,
                                leftEntry->second.getIntervals(),
                                rightEntry->second.getIntervals());
            return leftResult;
        }

        // Here we can combine if paths differ only by a Traverse element and both intervals
        // are the same, with array bounds. For example:
        //      Left:   Id,          [[1, 2, 3], [1, 2, 3]]
        //      Right:  Traverse Id  [[1, 2, 3], [1, 2, 3]]
        // We can then combine into:
        //    Traverse Id:           [[1, 2, 3], [1, 2, 3]] OR [1, 1]
        // We also need to retain the original filter.

        if (leftKey._projectionName != rightKey._projectionName) {
            return {};
        }
        if (leftReq.hasBoundProjectionName() || rightReq.hasBoundProjectionName()) {
            return {};
        }

        auto& leftIntervals = leftReq.getIntervals();
        auto& rightIntervals = rightReq.getIntervals();
        const auto& leftInterval = IntervalReqExpr::getSingularDNF(leftIntervals);
        const auto& rightInterval = IntervalReqExpr::getSingularDNF(rightIntervals);
        if (!leftInterval || !rightInterval || leftInterval != rightInterval) {
            return {};
        }
        if (!leftInterval->isEquality() || !rightInterval->isEquality()) {
            // For now only supporting equalities.
            return {};
        }

        const ABT& bound = leftInterval->getLowBound().getBound();
        const auto constBoundPtr = bound.cast<Constant>();
        if (constBoundPtr == nullptr) {
            return {};
        }
        const auto [tag, val] = constBoundPtr->get();
        if (tag != sbe::value::TypeTags::Array) {
            return {};
        }
        const sbe::value::Array* arr = sbe::value::getArrayView(val);
        if (arr->size() == 0) {
            // For now we do not support empty arrays. Need to translate into null bounds.
            return {};
        }

        const auto [elTag, elVal] = arr->getAt(0);
        const auto [elTagCopy, elValCopy] = sbe::value::copyValue(elTag, elVal);
        ABT elementBound = make<Constant>(elTagCopy, elValCopy);
        // Create new interval which uses the first element of the array.
        const IntervalReqExpr::Node& newInterval =
            IntervalReqExpr::makeSingularDNF(IntervalRequirement{
                {true /*inclusive*/, elementBound}, {true /*inclusive*/, elementBound}});

        const ABT& leftPath = leftKey._path;
        const ABT& rightPath = rightKey._path;
        if (const auto leftTraversePtr = leftPath.cast<PathTraverse>();
            leftTraversePtr != nullptr && leftTraversePtr->getPath().is<PathIdentity>() &&
            rightPath.is<PathIdentity>()) {
            // leftPath = Id, rightPath = Traverse Id.
            combineIntervalsDNF(false /*intersect*/, leftIntervals, newInterval);
            leftResult->_retainPredicate = true;
            return leftResult;
        } else if (const auto rightTraversePtr = rightPath.cast<PathTraverse>();
                   rightTraversePtr != nullptr && rightTraversePtr->getPath().is<PathIdentity>() &&
                   leftPath.is<PathIdentity>()) {
            // leftPath = Traverse Id, rightPath = Id.
            combineIntervalsDNF(false /*intersect*/, rightIntervals, newInterval);
            rightResult->_retainPredicate = true;
            return rightResult;
        }

        return {};
    }

    ResultType transport(const ABT& n,
                         const PathComposeM& pathComposeM,
                         ResultType leftResult,
                         ResultType rightResult) {
        if (!_isFilterContext) {
            return {};
        }

        return handleComposition(
            true /*isMultiplicative*/, std::move(leftResult), std::move(rightResult));
    }

    ResultType transport(const ABT& n,
                         const PathComposeA& pathComposeA,
                         ResultType leftResult,
                         ResultType rightResult) {
        if (!_isFilterContext) {
            return {};
        }

        const auto& path1 = pathComposeA.getPath1();
        const auto& path2 = pathComposeA.getPath2();
        const auto& eqNull = make<PathCompare>(Operations::Eq, Constant::null());
        const auto& pathDefault = make<PathDefault>(Constant::boolean(true));

        if ((path1 == eqNull && path2 == pathDefault) ||
            (path1 == pathDefault && path2 == eqNull)) {
            // In order to create null bound, we need to query for Nothing or Null.

            auto intervalExpr = IntervalReqExpr::makeSingularDNF(IntervalRequirement{
                {true /*inclusive*/, Constant::null()}, {true /*inclusive*/, Constant::null()}});
            return {{PartialSchemaRequirements{
                {PartialSchemaKey{},
                 PartialSchemaRequirement{"" /*boundProjectionName*/, std::move(intervalExpr)}}}}};
        }

        return handleComposition(
            false /*isMultiplicative*/, std::move(leftResult), std::move(rightResult));
    }

    template <class T>
    static ResultType handleGetAndTraverse(const ABT& n, ResultType inputResult) {
        if (!inputResult) {
            return {};
        }
        if (inputResult->_bound.has_value()) {
            return {};
        }

        // New map has keys with appended paths.
        PartialSchemaRequirements newMap;

        for (auto& entry : inputResult->_reqMap) {
            if (!entry.first._projectionName.empty()) {
                return {};
            }

            ABT path = entry.first._path;

            // Updated key path to be now rooted at n, with existing key path as child.
            ABT appendedPath = n;
            std::swap(appendedPath.cast<T>()->getPath(), path);
            std::swap(path, appendedPath);

            newMap.emplace(PartialSchemaKey{"", std::move(path)}, std::move(entry.second));
        }

        inputResult->_reqMap = std::move(newMap);
        return inputResult;
    }

    ResultType transport(const ABT& n, const PathGet& pathGet, ResultType inputResult) {
        return handleGetAndTraverse<PathGet>(n, std::move(inputResult));
    }

    ResultType transport(const ABT& n, const PathTraverse& pathTraverse, ResultType inputResult) {
        if (!inputResult) {
            return {};
        }
        if (inputResult->_reqMap.size() > 1) {
            // Cannot append traverse if we have more than one requirement.
            return {};
        }

        auto result = handleGetAndTraverse<PathTraverse>(n, std::move(inputResult));
        if (result) {
            result->_hasTraversed = true;
        }
        return result;
    }

    ResultType transport(const ABT& n, const PathCompare& pathCompare, ResultType inputResult) {
        if (!inputResult) {
            return {};
        }
        if (!inputResult->_bound || !inputResult->_reqMap.empty()) {
            return {};
        }

        const auto& bound = *inputResult->_bound;
        bool lowBoundInclusive = true;
        ABT lowBound = Constant::minKey();
        bool highBoundInclusive = true;
        ABT highBound = Constant::maxKey();

        const Operations op = pathCompare.op();
        switch (op) {
            case Operations::Eq:
                lowBound = bound;
                highBound = bound;
                break;

            case Operations::Lt:
            case Operations::Lte:
                highBoundInclusive = op == Operations::Lte;
                highBound = bound;
                break;

            case Operations::Gt:
            case Operations::Gte:
                lowBoundInclusive = op == Operations::Gte;
                lowBound = bound;
                break;

            default:
                // TODO handle other comparisons?
                return {};
        }

        auto intervalExpr = IntervalReqExpr::makeSingularDNF(IntervalRequirement{
            {lowBoundInclusive, std::move(lowBound)}, {highBoundInclusive, std::move(highBound)}});
        return {{PartialSchemaRequirements{
            {PartialSchemaKey{},
             PartialSchemaRequirement{"" /*boundProjectionName*/, std::move(intervalExpr)}}}}};
    }

    ResultType transport(const ABT& n, const PathIdentity& pathIdentity) {
        return {{PartialSchemaRequirements{{{}, {}}}}};
    }

    ResultType transport(const ABT& n, const Constant& c) {
        if (c.isNull()) {
            // Cannot create bounds with just NULL.
            return {};
        }
        return {{n}};
    }

    template <typename T, typename... Ts>
    ResultType transport(const ABT& n, const T& node, Ts&&...) {
        if constexpr (std::is_base_of_v<ExpressionSyntaxSort, T>) {
            // We allow expressions to participate in bounds.
            return {{n}};
        }
        // General case. Reject conversion.
        return {};
    }

    ResultType convert(const ABT& input) {
        return algebra::transport<true>(input, *this);
    }

private:
    const bool _isFilterContext;
};

boost::optional<PartialSchemaReqConversion> convertExprToPartialSchemaReq(
    const ABT& expr, const bool isFilterContext) {
    PartialSchemaReqConverter converter(isFilterContext);
    auto result = converter.convert(expr);
    if (!result || result->_reqMap.empty()) {
        return {};
    }

    for (const auto& entry : result->_reqMap) {
        if (entry.first.emptyPath() && isIntervalReqFullyOpenDNF(entry.second.getIntervals())) {
            // We need to determine either path or interval (or both).
            return {};
        }
    }
    return result;
}

static bool intersectPartialSchemaReq(PartialSchemaRequirements& reqMap,
                                      PartialSchemaKey key,
                                      PartialSchemaRequirement req,
                                      ProjectionRenames& projectionRenames) {
    for (;;) {
        bool merged = false;
        PartialSchemaKey newKey;
        PartialSchemaRequirement newReq;

        const bool reqHasBoundProj = req.hasBoundProjectionName();
        {
            // Look for exact match on the path, and if found combine intervals.
            auto it = reqMap.find(key);
            if (it != reqMap.cend()) {
                PartialSchemaRequirement& existingReq = it->second;
                if (reqHasBoundProj) {
                    if (existingReq.hasBoundProjectionName()) {
                        projectionRenames.emplace(req.getBoundProjectionName(),
                                                  existingReq.getBoundProjectionName());
                    } else {
                        existingReq.setBoundProjectionName(req.getBoundProjectionName());
                    }
                }
                combineIntervalsDNF(
                    true /*intersect*/, existingReq.getIntervals(), req.getIntervals());
                return true;
            }
        }

        for (auto it = reqMap.begin(); it != reqMap.cend();) {
            const auto& [existingKey, existingReq] = *it;
            uassert(6624150,
                    "Existing key referring to new requirement.",
                    !reqHasBoundProj ||
                        existingKey._projectionName != req.getBoundProjectionName());

            if (existingReq.hasBoundProjectionName() &&
                key._projectionName == existingReq.getBoundProjectionName()) {
                // The new key is referring to a projection the existing requirement binds.
                if (reqHasBoundProj) {
                    return false;
                }

                newKey = existingKey;
                newReq = req;

                PathAppender appender(key._path);
                appender.append(newKey._path);

                combineIntervalsDNF(
                    true /*intersect*/, newReq.getIntervals(), existingReq.getIntervals());

                if (key._path.is<PathIdentity>()) {
                    newReq.setBoundProjectionName(existingReq.getBoundProjectionName());
                    reqMap.erase(it++);
                } else if (!isIntervalReqFullyOpenDNF(existingReq.getIntervals())) {
                    return false;
                }

                merged = true;
                break;
            } else {
                it++;
                continue;
            }
        }

        if (merged) {
            key = std::move(newKey);
            req = std::move(newReq);
        } else {
            reqMap[key] = req;
            break;
        }
    };

    return true;
}

bool intersectPartialSchemaReq(PartialSchemaRequirements& target,
                               const PartialSchemaRequirements& source,
                               ProjectionRenames& projectionRenames) {
    for (const auto& [key, req] : source) {
        if (!intersectPartialSchemaReq(target, key, req, projectionRenames)) {
            return false;
        }
    }

    return true;
}

std::string encodeIndexKeyName(const size_t indexField) {
    std::ostringstream os;
    os << kIndexKeyPrefix << " " << indexField;
    return os.str();
}

size_t decodeIndexKeyName(const std::string& fieldName) {
    std::istringstream is(fieldName);

    std::string prefix;
    is >> prefix;
    uassert(6624151, "Invalid index key prefix", prefix == kIndexKeyPrefix);

    int key;
    is >> key;
    return key;
}

/**
 * Checks if one index path is a prefix of another. Considers only Get, Traverse, and Id.
 * Return the suffix that doesn't match.
 */
class PathSuffixExtactor {
public:
    using ResultType = boost::optional<ABT::reference_type>;

    /**
     * 'n' - The complete index path being compared to, can be modified if needed.
     * 'node' - Same as 'n' but cast to a specific type by the caller in order to invoke the
     *   correct operator.
     * 'other' - The query that is checked if it is a prefix of the index.
     */
    ResultType operator()(const ABT& n, const PathGet& node, const ABT& other) {
        if (auto otherGet = other.cast<PathGet>();
            otherGet != nullptr && otherGet->name() == node.name()) {
            if (auto otherChildTraverse = otherGet->getPath().cast<PathTraverse>();
                otherChildTraverse != nullptr && !node.getPath().is<PathTraverse>()) {
                // If a query path has a Traverse, but the index path doesn't, the query can
                // still be evaluated by this index. Skip the Traverse node, and continue matching.
                return node.getPath().visit(*this, otherChildTraverse->getPath());
            } else {
                return node.getPath().visit(*this, otherGet->getPath());
            }
        }
        return {};
    }

    ResultType operator()(const ABT& n, const PathTraverse& node, const ABT& other) {
        if (auto otherTraverse = other.cast<PathTraverse>();
            otherTraverse != nullptr && otherTraverse->getMaxDepth() == node.getMaxDepth()) {
            return node.getPath().visit(*this, otherTraverse->getPath());
        }
        return {};
    }

    ResultType operator()(const ABT& n, const PathIdentity& node, const ABT& other) {
        return {other.ref()};
    }

    template <typename T, typename... Ts>
    ResultType operator()(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        uasserted(6624152, "Unexpected node type");
    }

    static ResultType check(const ABT& node, const ABT& candidatePrefix) {
        PathSuffixExtactor instance;
        return candidatePrefix.visit(instance, node);
    }
};

/**
 * Check if a path contains a Traverse node over the last path component.
 */
class PathTraverseChecker {
public:
    PathTraverseChecker() {}

    /**
     * PathIdentity is always the last node in a path.
     * Return true if either 'node' is a PathTraverse just before the last node.
     * If the input Traverse node is somewhere in the middle of the path, return
     * the result of its child.
     */
    bool transport(const ABT& /*n*/, const PathTraverse& node, bool childResult) {
        return node.getPath().is<PathIdentity>() || childResult;
    }

    bool transport(const ABT& /*n*/, const PathGet& node, bool childResult) {
        return childResult;
    }

    bool transport(const ABT& /*n*/, const PathIdentity& node) {
        return false;
    }

    template <typename T, typename... Ts>
    bool transport(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        uasserted(6624153, "Index paths only consist of Get, Traverse, and Id nodes.");
        return false;
    }

    /**
     * Return true if the node before the last one in 'path' is a PathTraverse.
     * Return false otherwise.
     */
    bool check(const ABT& path) {
        return algebra::transport<true>(path, *this);
    }
};

void findMatchingSchemaRequirement(const PartialSchemaKey& indexKey,
                                   const PartialSchemaRequirements& reqMap,
                                   PartialSchemaKeySet& keySet,
                                   PartialSchemaRequirement& req,
                                   const bool setIntervalsAndBoundProj) {
    for (const auto& [queryKey, queryReq] : reqMap) {
        const auto pathSuffixResult = PathSuffixExtactor::check(queryKey._path, indexKey._path);
        if (pathSuffixResult.has_value() && pathSuffixResult->is<PathIdentity>()) {
            keySet.insert(queryKey);

            if (setIntervalsAndBoundProj) {
                // Combine all matching requirements into a single entry.
                if (queryReq.hasBoundProjectionName()) {
                    uassert(6624154, "Unexpected bound projection", !req.hasBoundProjectionName());
                    req.setBoundProjectionName(queryReq.getBoundProjectionName());
                }
                if (!isIntervalReqFullyOpenDNF(queryReq.getIntervals())) {
                    uassert(6624350,
                            "Unexpected intervals",
                            isIntervalReqFullyOpenDNF(req.getIntervals()));
                    req.getIntervals() = queryReq.getIntervals();
                }
            }
        }
    }
}

CandidateIndexMap computeCandidateIndexMap(PrefixId& prefixId,
                                           const ProjectionName& scanProjectionName,
                                           const PartialSchemaRequirements& reqMap,
                                           const ScanDefinition& scanDef,
                                           bool& hasEmptyInterval) {
    CandidateIndexMap result;
    hasEmptyInterval = false;

    for (const auto& [indexDefName, indexDef] : scanDef.getIndexDefs()) {
        FieldProjectionMap indexProjectionMap;
        auto intervals = MultiKeyIntervalReqExpr::makeSingularDNF();  // Singular empty interval.
        PartialSchemaRequirements residualRequirements;
        ProjectionNameSet residualRequirementsTempProjections;
        ResidualKeyMap residualKeyMap;
        opt::unordered_set<size_t> fieldsToCollate;
        size_t intervalPrefixSize = 0;

        PartialSchemaKeySet unsatisfiedKeys;
        for (const auto& [key, req] : reqMap) {
            unsatisfiedKeys.insert(key);
        }

        // True if the paths from partial schema requirements form a strict prefix of the index
        // collation.
        bool isPrefix = true;
        // If we formed bounds using at least one requirement (as opposed to having only residual
        // requirements).
        bool hasExactMatch = false;
        bool indexSuitable = true;

        const IndexCollationSpec& indexCollationSpec = indexDef.getCollationSpec();
        for (size_t indexField = 0; indexField < indexCollationSpec.size(); indexField++) {
            const auto& indexCollationEntry = indexCollationSpec.at(indexField);
            PartialSchemaKey indexKey{scanProjectionName, indexCollationEntry._path};

            PartialSchemaKeySet keySet;
            PartialSchemaRequirement req;
            findMatchingSchemaRequirement(indexKey, reqMap, keySet, req);
            if (!keySet.empty() && isPrefix) {
                hasExactMatch = true;
                const PartialSchemaKey& queryKey = *keySet.begin();
                for (const auto& key : keySet) {
                    unsatisfiedKeys.erase(key);
                }

                // The interval derived from the query (not from the index).
                const auto& requiredInterval = req.getIntervals();

                PathTraverseChecker pathChecker;
                const bool indexPathContainsArrays = pathChecker.check(indexCollationEntry._path);
                bool combineSuccess = false;
                if (indexPathContainsArrays) {
                    combineSuccess = combineMultiKeyIntervalsDNF(intervals, requiredInterval);
                } else {
                    auto intersectedIntervals = intersectDNFIntervals(requiredInterval);
                    if (intersectedIntervals.has_value()) {
                        combineSuccess =
                            combineMultiKeyIntervalsDNF(intervals, intersectedIntervals.get());
                    } else {
                        if (indexDef.getPartialReqMap().empty()) {
                            hasEmptyInterval = true;
                            return {};
                        } else {
                            // This is a partial index, so skip the empty interval, but consider the
                            // remaining indexes.
                            indexSuitable = false;
                            break;
                        }
                    }
                }

                if (combineSuccess) {
                    intervalPrefixSize++;
                } else {
                    if (!combineMultiKeyIntervalsDNF(intervals,
                                                     IntervalReqExpr::makeSingularDNF())) {
                        uasserted(6624155, "Cannot combine with an open interval");
                    }

                    // Move interval into residual requirements.
                    if (req.hasBoundProjectionName()) {
                        PartialSchemaKey residualKey{req.getBoundProjectionName(),
                                                     make<PathIdentity>()};
                        residualRequirements.emplace(
                            residualKey, PartialSchemaRequirement{"", req.getIntervals()});
                        residualKeyMap.emplace(std::move(residualKey), queryKey);
                        residualRequirementsTempProjections.insert(req.getBoundProjectionName());
                    } else {
                        ProjectionName tempProj = prefixId.getNextId("evalTemp");
                        PartialSchemaKey residualKey{tempProj, make<PathIdentity>()};
                        residualRequirements.emplace(residualKey, req);
                        residualKeyMap.emplace(std::move(residualKey), queryKey);
                        residualRequirementsTempProjections.insert(tempProj);

                        // Include bounds projection into index spec.
                        if (!indexProjectionMap._fieldProjections
                                 .emplace(encodeIndexKeyName(indexField), std::move(tempProj))
                                 .second) {
                            uasserted(6624156, "Duplicate field name");
                        }
                    }
                }

                // Include bounds projection into index spec.
                if (req.hasBoundProjectionName() &&
                    !indexProjectionMap._fieldProjections
                         .emplace(encodeIndexKeyName(indexField), req.getBoundProjectionName())
                         .second) {
                    uasserted(6624157, "Duplicate field name");
                }

                if (auto singularInterval = IntervalReqExpr::getSingularDNF(requiredInterval);
                    !singularInterval || !singularInterval->isEquality()) {
                    // We only care about collation of for non-equality intervals.
                    // Equivalently, it is sufficient for singular intervals to be clustered.
                    fieldsToCollate.insert(indexField);
                }
            } else {
                bool foundPathPrefix = false;
                for (const auto& queryKey : unsatisfiedKeys) {
                    const auto pathPrefixResult =
                        PathSuffixExtactor::check(queryKey._path, indexCollationEntry._path);
                    if (pathPrefixResult.has_value()) {
                        ProjectionName tempProj = prefixId.getNextId("evalTemp");
                        PartialSchemaKey residualKey{tempProj, pathPrefixResult.get()};
                        residualRequirements.emplace(residualKey, reqMap.at(queryKey));
                        residualKeyMap.emplace(std::move(residualKey), queryKey);
                        residualRequirementsTempProjections.insert(tempProj);

                        // Include bounds projection into index spec.
                        if (!indexProjectionMap._fieldProjections
                                 .emplace(encodeIndexKeyName(indexField), std::move(tempProj))
                                 .second) {
                            uasserted(6624158, "Duplicate field name");
                        }

                        if (!combineMultiKeyIntervalsDNF(intervals,
                                                         IntervalReqExpr::makeSingularDNF())) {
                            uasserted(6624159, "Cannot combine with an open interval");
                        }

                        foundPathPrefix = true;
                        unsatisfiedKeys.erase(queryKey);
                        break;
                    }
                }

                if (!foundPathPrefix) {
                    isPrefix = false;
                    if (!combineMultiKeyIntervalsDNF(intervals,
                                                     IntervalReqExpr::makeSingularDNF())) {
                        uasserted(6624160, "Cannot combine with an open interval");
                    }
                }
            }
        }
        if (!indexSuitable) {
            continue;
        }
        if (!hasExactMatch) {
            continue;
        }
        if (!unsatisfiedKeys.empty()) {
            continue;
        }

        uassert(6624161, "Invalid map sizes", residualRequirements.size() == residualKeyMap.size());
        result.emplace(indexDefName,
                       CandidateIndexEntry{std::move(indexProjectionMap),
                                           std::move(intervals),
                                           std::move(residualRequirements),
                                           std::move(residualRequirementsTempProjections),
                                           std::move(residualKeyMap),
                                           std::move(fieldsToCollate),
                                           intervalPrefixSize});
    }

    return result;
}

class PartialSchemaReqLowerTransport {
public:
    PartialSchemaReqLowerTransport(const bool hasBoundProjName)
        : _hasBoundProjName(hasBoundProjName) {}

    ABT transport(const IntervalReqExpr::Atom& node) {
        const auto& interval = node.getExpr();
        const auto& lowBound = interval.getLowBound();
        const auto& highBound = interval.getHighBound();

        if (interval.isEquality()) {
            if (auto constPtr = lowBound.getBound().cast<Constant>()) {
                if (constPtr->isNull()) {
                    uassert(6624163,
                            "Cannot lower null index bound with bound projection",
                            !_hasBoundProjName);
                    return make<PathComposeA>(make<PathDefault>(Constant::boolean(true)),
                                              make<PathCompare>(Operations::Eq, Constant::null()));
                }
                return make<PathCompare>(Operations::Eq, lowBound.getBound());
            } else {
                uassert(6624164,
                        "Cannot lower variable index bound with bound projection",
                        !_hasBoundProjName);
                return make<PathCompare>(Operations::Eq, lowBound.getBound());
            }
        }

        ABT result = make<PathIdentity>();
        if (!lowBound.isMinusInf()) {
            maybeComposePath(
                result,
                make<PathCompare>(lowBound.isInclusive() ? Operations::Gte : Operations::Gt,
                                  lowBound.getBound()));
        }
        if (!highBound.isPlusInf()) {
            maybeComposePath(
                result,
                make<PathCompare>(highBound.isInclusive() ? Operations::Lte : Operations::Lt,
                                  highBound.getBound()));
        }
        return result;
    }

    template <class Element>
    ABT composeChildren(ABTVector childResults) {
        ABT result = make<PathIdentity>();
        for (ABT& n : childResults) {
            maybeComposePath<Element>(result, std::move(n));
        }
        return result;
    }

    ABT transport(const IntervalReqExpr::Conjunction& node, ABTVector childResults) {
        return composeChildren<PathComposeM>(std::move(childResults));
    }

    ABT transport(const IntervalReqExpr::Disjunction& node, ABTVector childResults) {
        return composeChildren<PathComposeA>(std::move(childResults));
    }

    ABT lower(const IntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }

private:
    const bool _hasBoundProjName;
};

void lowerPartialSchemaRequirement(const PartialSchemaKey& key,
                                   const PartialSchemaRequirement& req,
                                   ABT& node,
                                   const std::function<void(const ABT& node)>& visitor) {
    const bool hasBoundProjName = req.hasBoundProjectionName();
    PartialSchemaReqLowerTransport transport(hasBoundProjName);
    ABT path = transport.lower(req.getIntervals());
    const bool pathIsId = path.is<PathIdentity>();

    if (hasBoundProjName) {
        node = make<EvaluationNode>(req.getBoundProjectionName(),
                                    make<EvalPath>(key._path, make<Variable>(key._projectionName)),
                                    std::move(node));
        visitor(node);

        if (!pathIsId) {
            node = make<FilterNode>(
                make<EvalFilter>(std::move(path), make<Variable>(req.getBoundProjectionName())),
                std::move(node));
            visitor(node);
        }
    } else {
        uassert(
            6624162, "If we do not have a bound projection, then we have a proper path", !pathIsId);

        PathAppender appender(std::move(path));
        path = key._path;
        appender.append(path);

        node =
            make<FilterNode>(make<EvalFilter>(std::move(path), make<Variable>(key._projectionName)),
                             std::move(node));
        visitor(node);
    }
}

void lowerPartialSchemaRequirements(const CEType baseCE,
                                    const CEType scanGroupCE,
                                    ResidualRequirements& requirements,
                                    ABT& physNode,
                                    NodeCEMap& nodeCEMap) {
    sortResidualRequirements(requirements);

    CEType residualCE = baseCE;
    for (const auto& [residualKey, residualReq, ce] : requirements) {
        if (scanGroupCE > 0.0) {
            residualCE *= ce / scanGroupCE;
        }
        lowerPartialSchemaRequirement(residualKey, residualReq, physNode, [&](const ABT& node) {
            nodeCEMap.emplace(node.cast<Node>(), residualCE);
        });
    }
}

void computePhysicalScanParams(PrefixId& prefixId,
                               const PartialSchemaRequirements& reqMap,
                               const PartialSchemaKeyCE& partialSchemaKeyCEMap,
                               const ProjectionNameOrderPreservingSet& requiredProjections,
                               ResidualRequirements& residualRequirements,
                               ProjectionRenames& projectionRenames,
                               FieldProjectionMap& fieldProjectionMap,
                               bool& requiresRootProjection) {
    for (const auto& [key, req] : reqMap) {
        bool hasBoundProjection = req.hasBoundProjectionName();
        if (hasBoundProjection && !requiredProjections.find(req.getBoundProjectionName()).second) {
            // Bound projection is not required, pretend we don't bind.
            hasBoundProjection = false;
        }
        if (!hasBoundProjection && isIntervalReqFullyOpenDNF(req.getIntervals())) {
            // Redundant requirement.
            continue;
        }

        const CEType keyCE = partialSchemaKeyCEMap.at(key);
        if (auto pathGet = key._path.cast<PathGet>(); pathGet != nullptr) {
            // Extract a new requirements path with removed simple paths.
            // For example if we have a key Get "a" Traverse Compare = 0 we leave only
            // Traverse Compare 0.
            if (pathGet->getPath().is<PathIdentity>() && hasBoundProjection) {
                const auto [it, inserted] = fieldProjectionMap._fieldProjections.emplace(
                    pathGet->name(), req.getBoundProjectionName());
                if (!inserted) {
                    projectionRenames.emplace(req.getBoundProjectionName(), it->second);
                }

                if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                    residualRequirements.emplace_back(
                        PartialSchemaKey{req.getBoundProjectionName(), make<PathIdentity>()},
                        PartialSchemaRequirement{"", req.getIntervals()},
                        keyCE);
                }
            } else {
                ProjectionName tempProjName;
                auto it = fieldProjectionMap._fieldProjections.find(pathGet->name());
                if (it == fieldProjectionMap._fieldProjections.cend()) {
                    tempProjName = prefixId.getNextId("evalTemp");
                    fieldProjectionMap._fieldProjections.emplace(pathGet->name(), tempProjName);
                } else {
                    tempProjName = it->second;
                }

                residualRequirements.emplace_back(
                    PartialSchemaKey{std::move(tempProjName), pathGet->getPath()}, req, keyCE);
            }
        } else {
            // Move other conditions into the residual map.
            requiresRootProjection = true;
            residualRequirements.emplace_back(key, req, keyCE);
        }
    }
}

void sortResidualRequirements(ResidualRequirements& residualReq) {
    // Sort residual requirements by estimated CE.
    std::sort(
        residualReq.begin(),
        residualReq.end(),
        [](const ResidualRequirement& x, const ResidualRequirement& y) { return x._ce < y._ce; });
}

void applyProjectionRenames(ProjectionRenames projectionRenames,
                            ABT& node,
                            const std::function<void(const ABT& node)>& visitor) {
    for (auto&& [targetProjName, sourceProjName] : projectionRenames) {
        node = make<EvaluationNode>(
            std::move(targetProjName), make<Variable>(std::move(sourceProjName)), std::move(node));
        visitor(node);
    }
}

ABT lowerRIDIntersectGroupBy(PrefixId& prefixId,
                             const ProjectionName& ridProjName,
                             const CEType intersectedCE,
                             const CEType leftCE,
                             const CEType rightCE,
                             const properties::PhysProps& physProps,
                             const properties::PhysProps& leftPhysProps,
                             const properties::PhysProps& rightPhysProps,
                             ABT leftChild,
                             ABT rightChild,
                             NodeCEMap& nodeCEMap,
                             ChildPropsType& childProps) {
    using namespace properties;

    const auto& leftProjections =
        getPropertyConst<ProjectionRequirement>(leftPhysProps).getProjections();

    ABTVector aggExpressions;
    ProjectionNameVector aggProjectionNames;

    const ProjectionName sideIdProjectionName = prefixId.getNextId("sideId");
    const ProjectionName sideSetProjectionName = prefixId.getNextId("sides");

    aggExpressions.emplace_back(
        make<FunctionCall>("$addToSet", makeSeq(make<Variable>(sideIdProjectionName))));
    aggProjectionNames.push_back(sideSetProjectionName);

    leftChild =
        make<EvaluationNode>(sideIdProjectionName, Constant::int64(0), std::move(leftChild));
    childProps.emplace_back(&leftChild.cast<EvaluationNode>()->getChild(), leftPhysProps);
    nodeCEMap.emplace(leftChild.cast<Node>(), leftCE);

    rightChild =
        make<EvaluationNode>(sideIdProjectionName, Constant::int64(1), std::move(rightChild));
    childProps.emplace_back(&rightChild.cast<EvaluationNode>()->getChild(), rightPhysProps);
    nodeCEMap.emplace(rightChild.cast<Node>(), rightCE);

    ProjectionNameVector sortedProjections =
        getPropertyConst<ProjectionRequirement>(physProps).getProjections().getVector();
    std::sort(sortedProjections.begin(), sortedProjections.end());

    ProjectionNameVector unionProjections{ridProjName, sideIdProjectionName};
    for (const ProjectionName& projectionName : sortedProjections) {
        if (projectionName == ridProjName) {
            continue;
        }

        ProjectionName tempProjectionName = prefixId.getNextId("unionTemp");
        unionProjections.push_back(tempProjectionName);

        if (leftProjections.find(projectionName).second) {
            leftChild = make<EvaluationNode>(
                tempProjectionName, make<Variable>(projectionName), std::move(leftChild));
            nodeCEMap.emplace(leftChild.cast<Node>(), leftCE);

            rightChild = make<EvaluationNode>(
                tempProjectionName, Constant::nothing(), std::move(rightChild));
            nodeCEMap.emplace(rightChild.cast<Node>(), rightCE);
        } else {
            leftChild =
                make<EvaluationNode>(tempProjectionName, Constant::nothing(), std::move(leftChild));
            nodeCEMap.emplace(leftChild.cast<Node>(), leftCE);

            rightChild = make<EvaluationNode>(
                tempProjectionName, make<Variable>(projectionName), std::move(rightChild));
            nodeCEMap.emplace(rightChild.cast<Node>(), rightCE);
        }

        aggExpressions.emplace_back(
            make<FunctionCall>("$max", makeSeq(make<Variable>(tempProjectionName))));
        aggProjectionNames.push_back(projectionName);
    }

    ABT result = make<UnionNode>(std::move(unionProjections),
                                 makeSeq(std::move(leftChild), std::move(rightChild)));
    nodeCEMap.emplace(result.cast<Node>(), leftCE + rightCE);

    result = make<GroupByNode>(ProjectionNameVector{ridProjName},
                               std::move(aggProjectionNames),
                               std::move(aggExpressions),
                               std::move(result));
    nodeCEMap.emplace(result.cast<Node>(), intersectedCE);

    result = make<FilterNode>(
        make<EvalFilter>(
            make<PathCompare>(Operations::Eq, Constant::int64(2)),
            make<FunctionCall>("getArraySize", makeSeq(make<Variable>(sideSetProjectionName)))),
        std::move(result));
    nodeCEMap.emplace(result.cast<Node>(), intersectedCE);

    return result;
}

ABT lowerRIDIntersectHashJoin(PrefixId& prefixId,
                              const ProjectionName& ridProjName,
                              const CEType intersectedCE,
                              const CEType leftCE,
                              const CEType rightCE,
                              const properties::PhysProps& leftPhysProps,
                              const properties::PhysProps& rightPhysProps,
                              ABT leftChild,
                              ABT rightChild,
                              NodeCEMap& nodeCEMap,
                              ChildPropsType& childProps) {
    using namespace properties;

    ProjectionName rightRIDProjName = prefixId.getNextId("rid");
    rightChild =
        make<EvaluationNode>(rightRIDProjName, make<Variable>(ridProjName), std::move(rightChild));
    ABT* rightChildPtr = &rightChild.cast<EvaluationNode>()->getChild();
    nodeCEMap.emplace(rightChild.cast<Node>(), rightCE);

    auto rightProjections =
        getPropertyConst<ProjectionRequirement>(rightPhysProps).getProjections();
    rightProjections.erase(ridProjName);
    rightProjections.emplace_back(rightRIDProjName);
    ProjectionNameVector sortedProjections = rightProjections.getVector();
    std::sort(sortedProjections.begin(), sortedProjections.end());

    // Use a union node to restrict the rid projection name coming from the right child in order
    // to ensure we do not have the same rid from both children. This node is optimized away
    // during lowering.
    rightChild = make<UnionNode>(std::move(sortedProjections), makeSeq(std::move(rightChild)));
    nodeCEMap.emplace(rightChild.cast<Node>(), rightCE);

    ABT result = make<HashJoinNode>(JoinType::Inner,
                                    ProjectionNameVector{ridProjName},
                                    ProjectionNameVector{std::move(rightRIDProjName)},
                                    std::move(leftChild),
                                    std::move(rightChild));
    nodeCEMap.emplace(result.cast<Node>(), intersectedCE);

    childProps.emplace_back(&result.cast<HashJoinNode>()->getLeftChild(), leftPhysProps);
    childProps.emplace_back(rightChildPtr, rightPhysProps);

    return result;
}

ABT lowerRIDIntersectMergeJoin(PrefixId& prefixId,
                               const ProjectionName& ridProjName,
                               const CEType intersectedCE,
                               const CEType leftCE,
                               const CEType rightCE,
                               const properties::PhysProps& leftPhysProps,
                               const properties::PhysProps& rightPhysProps,
                               ABT leftChild,
                               ABT rightChild,
                               NodeCEMap& nodeCEMap,
                               ChildPropsType& childProps) {
    using namespace properties;

    ProjectionName rightRIDProjName = prefixId.getNextId("rid");
    rightChild =
        make<EvaluationNode>(rightRIDProjName, make<Variable>(ridProjName), std::move(rightChild));
    ABT* rightChildPtr = &rightChild.cast<EvaluationNode>()->getChild();
    nodeCEMap.emplace(rightChild.cast<Node>(), rightCE);

    auto rightProjections =
        getPropertyConst<ProjectionRequirement>(rightPhysProps).getProjections();
    rightProjections.erase(ridProjName);
    rightProjections.emplace_back(rightRIDProjName);
    ProjectionNameVector sortedProjections = rightProjections.getVector();
    std::sort(sortedProjections.begin(), sortedProjections.end());

    // Use a union node to restrict the rid projection name coming from the right child in order
    // to ensure we do not have the same rid from both children. This node is optimized away
    // during lowering.
    rightChild = make<UnionNode>(std::move(sortedProjections), makeSeq(std::move(rightChild)));
    nodeCEMap.emplace(rightChild.cast<Node>(), rightCE);

    ABT result = make<MergeJoinNode>(ProjectionNameVector{ridProjName},
                                     ProjectionNameVector{std::move(rightRIDProjName)},
                                     std::vector<CollationOp>{CollationOp::Ascending},
                                     std::move(leftChild),
                                     std::move(rightChild));
    nodeCEMap.emplace(result.cast<Node>(), intersectedCE);

    childProps.emplace_back(&result.cast<MergeJoinNode>()->getLeftChild(), leftPhysProps);
    childProps.emplace_back(rightChildPtr, rightPhysProps);

    return result;
}

class IntervalLowerTransport {
public:
    IntervalLowerTransport(PrefixId& prefixId,
                           const ProjectionName& ridProjName,
                           FieldProjectionMap indexProjectionMap,
                           const std::string& scanDefName,
                           const std::string& indexDefName,
                           const bool reverseOrder,
                           const CEType indexCE,
                           const CEType scanGroupCE,
                           NodeCEMap& nodeCEMap)
        : _prefixId(prefixId),
          _ridProjName(ridProjName),
          _scanDefName(scanDefName),
          _indexDefName(indexDefName),
          _reverseOrder(reverseOrder),
          _scanGroupCE(scanGroupCE),
          _nodeCEMap(nodeCEMap) {
        const SelectivityType indexSel = (scanGroupCE == 0.0) ? 0.0 : (indexCE / _scanGroupCE);
        _estimateStack.push_back(indexSel);
        _fpmStack.push_back(std::move(indexProjectionMap));
    };

    ABT transport(const MultiKeyIntervalReqExpr::Atom& node) {
        ABT physicalIndexScan = make<IndexScanNode>(
            _fpmStack.back(),
            IndexSpecification{_scanDefName, _indexDefName, node.getExpr(), _reverseOrder});
        _nodeCEMap.emplace(physicalIndexScan.cast<Node>(), _scanGroupCE * _estimateStack.back());
        return physicalIndexScan;
    }

    template <bool isConjunction>
    void prepare(const size_t childCount) {
        // Here we are assuming each conjunction and disjunction contribute uniformly to the total
        // selectivity.
        // TODO: consider estimates per individual interval.

        const SelectivityType parentSel = _estimateStack.back();
        SelectivityType childSel = 0.0;
        if constexpr (isConjunction) {
            childSel = (parentSel == 0.0) ? 0.0 : std::pow(parentSel, 1.0 / childCount);
        } else {
            childSel = _estimateStack.back() / childCount;
        }
        _estimateStack.push_back(childSel);

        FieldProjectionMap childMap = _fpmStack.back();
        if (childMap._ridProjection.empty()) {
            childMap._ridProjection = _ridProjName;
        }
        if (childCount > 1) {
            for (auto& [fieldName, projectionName] : childMap._fieldProjections) {
                projectionName = _prefixId.getNextId(isConjunction ? "conjunction" : "disjunction");
            }
        }
        _fpmStack.push_back(std::move(childMap));
    }

    void prepare(const MultiKeyIntervalReqExpr::Conjunction& node) {
        prepare<true /*isConjunction*/>(node.nodes().size());
    }

    template <bool isIntersect>
    ABT implement(ABTVector inputs) {
        _estimateStack.pop_back();
        const CEType ce = _scanGroupCE * _estimateStack.back();

        auto innerMap = std::move(_fpmStack.back());
        _fpmStack.pop_back();
        auto outerMap = _fpmStack.back();

        const size_t inputSize = inputs.size();
        if (inputSize == 1) {
            return std::move(inputs.front());
        }

        ProjectionNameVector unionProjectionNames;
        unionProjectionNames.push_back(innerMap._ridProjection);
        for (const auto& [fieldName, projectionName] : innerMap._fieldProjections) {
            unionProjectionNames.push_back(projectionName);
        }

        ProjectionNameVector aggProjectionNames;
        for (const auto& [fieldName, projectionName] : outerMap._fieldProjections) {
            aggProjectionNames.push_back(projectionName);
        }

        ABTVector aggExpressions;
        for (const auto& [fieldName, projectionName] : innerMap._fieldProjections) {
            aggExpressions.emplace_back(
                make<FunctionCall>("$first", makeSeq(make<Variable>(projectionName))));
        }

        ProjectionName sideSetProjectionName;
        if constexpr (isIntersect) {
            const ProjectionName sideIdProjectionName = _prefixId.getNextId("sideId");
            unionProjectionNames.push_back(sideIdProjectionName);
            sideSetProjectionName = _prefixId.getNextId("sides");

            for (size_t index = 0; index < inputSize; index++) {
                ABT& input = inputs.at(index);
                input = make<EvaluationNode>(
                    sideIdProjectionName, Constant::int64(index), std::move(input));
                // Not relevant for cost.
                _nodeCEMap.emplace(input.cast<Node>(), 0.0);
            }

            aggExpressions.emplace_back(
                make<FunctionCall>("$addToSet", makeSeq(make<Variable>(sideIdProjectionName))));
            aggProjectionNames.push_back(sideSetProjectionName);
        }

        ABT result = make<UnionNode>(std::move(unionProjectionNames), std::move(inputs));
        _nodeCEMap.emplace(result.cast<Node>(), ce);

        result = make<GroupByNode>(ProjectionNameVector{innerMap._ridProjection},
                                   std::move(aggProjectionNames),
                                   std::move(aggExpressions),
                                   std::move(result));
        _nodeCEMap.emplace(result.cast<Node>(), ce);

        if constexpr (isIntersect) {
            result = make<FilterNode>(
                make<EvalFilter>(
                    make<PathCompare>(Operations::Eq, Constant::int64(inputSize)),
                    make<FunctionCall>("getArraySize",
                                       makeSeq(make<Variable>(sideSetProjectionName)))),
                std::move(result));
            _nodeCEMap.emplace(result.cast<Node>(), ce);
        }
        return result;
    }

    ABT transport(const MultiKeyIntervalReqExpr::Conjunction& node, ABTVector childResults) {
        return implement<true /*isIntersect*/>(std::move(childResults));
    }

    void prepare(const MultiKeyIntervalReqExpr::Disjunction& node) {
        prepare<false /*isConjunction*/>(node.nodes().size());
    }

    ABT transport(const MultiKeyIntervalReqExpr::Disjunction& node, ABTVector childResults) {
        return implement<false /*isIntersect*/>(std::move(childResults));
    }

    ABT lower(const MultiKeyIntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }

private:
    PrefixId& _prefixId;
    const ProjectionName& _ridProjName;
    const std::string& _scanDefName;
    const std::string& _indexDefName;
    const bool _reverseOrder;
    const CEType _scanGroupCE;
    NodeCEMap& _nodeCEMap;

    std::vector<SelectivityType> _estimateStack;
    std::vector<FieldProjectionMap> _fpmStack;
};

ABT lowerIntervals(PrefixId& prefixId,
                   const ProjectionName& ridProjName,
                   FieldProjectionMap indexProjectionMap,
                   const std::string& scanDefName,
                   const std::string& indexDefName,
                   const MultiKeyIntervalReqExpr::Node& intervals,
                   const bool reverseOrder,
                   const CEType indexCE,
                   const CEType scanGroupCE,
                   NodeCEMap& nodeCEMap) {
    IntervalLowerTransport lowerTransport(prefixId,
                                          ridProjName,
                                          std::move(indexProjectionMap),
                                          scanDefName,
                                          indexDefName,
                                          reverseOrder,
                                          indexCE,
                                          scanGroupCE,
                                          nodeCEMap);
    return lowerTransport.lower(intervals);
}

}  // namespace mongo::optimizer
