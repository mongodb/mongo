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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo::optimizer {

std::vector<ABT::reference_type> collectComposed(const ABT& n) {
    if (auto comp = n.cast<PathComposeM>(); comp) {
        auto lhs = collectComposed(comp->getPath1());
        auto rhs = collectComposed(comp->getPath2());
        lhs.insert(lhs.end(), rhs.begin(), rhs.end());
        return lhs;
    }
    return {n.ref()};
}

// Helper function to count the size of a nested conjunction.
size_t countComposed(const ABT& n) {
    if (auto comp = n.cast<PathComposeM>()) {
        return countComposed(comp->getPath1()) + countComposed(comp->getPath2());
    }
    return 1;
}

std::vector<ABT::reference_type> collectComposedBounded(const ABT& n, size_t maxDepth) {
    if (countComposed(n) > maxDepth) {
        return {n.ref()};
    }
    return collectComposed(n);
}

bool isSimplePath(const ABT& node) {
    if (auto getPtr = node.cast<PathGet>();
        getPtr != nullptr && getPtr->getPath().is<PathIdentity>()) {
        return true;
    }
    return false;
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

properties::LogicalProps createInitialScanProps(const ProjectionName& projectionName,
                                                const std::string& scanDefName,
                                                const GroupIdType groupId,
                                                properties::DistributionSet distributions) {
    return makeLogicalProps(properties::IndexingAvailability(groupId,
                                                             projectionName,
                                                             scanDefName,
                                                             true /*eqPredsOnly*/,
                                                             false /*hasProperInterval*/,
                                                             {} /*satisfiedPartialIndexes*/),
                            properties::CollectionAvailability({scanDefName}),
                            properties::DistributionAvailability(std::move(distributions)));
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

CollationSplitResult splitCollationSpec(const boost::optional<ProjectionName>& ridProjName,
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

PartialSchemaReqConversion::PartialSchemaReqConversion(PartialSchemaRequirements reqMap)
    : _bound(),
      _reqMap(std::move(reqMap)),
      _hasIntersected(false),
      _hasTraversed(false),
      _retainPredicate(false) {}

PartialSchemaReqConversion::PartialSchemaReqConversion(ABT bound)
    : _bound(std::move(bound)),
      _reqMap(),
      _hasIntersected(false),
      _hasTraversed(false),
      _retainPredicate(false) {}

/**
 * Helper class that builds PartialSchemaRequirements property from an EvalFilter or an EvalPath.
 */
class PartialSchemaReqConverter {
public:
    using ResultType = boost::optional<PartialSchemaReqConversion>;

    PartialSchemaReqConverter(const bool isFilterContext, const PathToIntervalFn& pathToInterval)
        : _isFilterContext(isFilterContext), _pathToInterval(pathToInterval) {}

    ResultType handleEvalPathAndEvalFilter(ResultType pathResult, ResultType inputResult) {
        if (!pathResult || !inputResult) {
            return {};
        }
        if (pathResult->_bound || !inputResult->_bound || !inputResult->_reqMap.empty()) {
            return {};
        }

        if (auto boundPtr = inputResult->_bound->cast<Variable>(); boundPtr != nullptr) {
            const ProjectionName& boundVarName = boundPtr->name();
            PartialSchemaRequirements newMap;

            for (auto& [key, req] : pathResult->_reqMap) {
                if (key._projectionName) {
                    return {};
                }
                newMap.emplace(PartialSchemaKey{boundVarName, key._path}, std::move(req));
            }

            PartialSchemaReqConversion result{std::move(newMap)};
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

    template <bool isMultiplicative>
    static ResultType handleComposition(ResultType leftResult, ResultType rightResult) {
        const bool leftHasReqMap = leftResult && !leftResult->_bound;
        const bool rightHasReqMap = rightResult && !rightResult->_bound;
        if (!leftHasReqMap && !rightHasReqMap) {
            // Neither side is sargable.
            return {};
        }
        if constexpr (isMultiplicative) {
            // If one side is sargable but not both, we can keep just the sargable side.
            // This is a looser predicate than the original (because X >= (X AND Y)), so
            // keep the original.
            if (!leftHasReqMap) {
                rightResult->_retainPredicate = true;
                return rightResult;
            }
            if (!rightHasReqMap) {
                leftResult->_retainPredicate = true;
                return leftResult;
            }
        } else {
            if (!leftHasReqMap || !rightHasReqMap) {
                return {};
            }
        }

        auto& leftReqMap = leftResult->_reqMap;
        auto& rightReqMap = rightResult->_reqMap;
        if constexpr (isMultiplicative) {
            ProjectionRenames projectionRenames;
            if (!intersectPartialSchemaReq(leftReqMap, rightReqMap, projectionRenames)) {
                return {};
            }
            if (!projectionRenames.empty()) {
                return {};
            }

            leftResult->_hasIntersected = true;
            return leftResult;
        }
        // Additive composition: we never have projections in this case; only predicates.
        for (const auto& [key, req] : leftReqMap) {
            tassert(7155021,
                    "Unexpected binding in ComposeA in PartialSchemaReqConverter",
                    !req.getBoundProjectionName());
        }
        for (const auto& [key, req] : rightReqMap) {
            tassert(7155022,
                    "Unexpected binding in ComposeA in PartialSchemaReqConverter",
                    !req.getBoundProjectionName());
        }

        auto leftEntry = leftReqMap.begin();
        auto rightEntry = rightReqMap.begin();
        auto& [leftKey, leftReq] = *leftEntry;
        auto& [rightKey, rightReq] = *rightEntry;

        {
            // Check if the left and right requirements are all or none perf-only.
            size_t perfOnlyCount = 0;
            for (const auto& [key, req] : leftReqMap) {
                if (req.getIsPerfOnly()) {
                    perfOnlyCount++;
                }
            }
            for (const auto& [key, req] : rightReqMap) {
                if (req.getIsPerfOnly()) {
                    perfOnlyCount++;
                }
            }
            if (perfOnlyCount != 0 && perfOnlyCount != leftReqMap.size() + rightReqMap.size()) {
                // For now allow only predicates with the same perf-only flag.
                return {};
            }
        }

        if (leftReqMap.count(leftKey) == leftReqMap.size() &&
            rightReqMap.count(leftKey) == rightReqMap.size()) {
            // All reqs from both sides use the same key (input binding + path).

            // Each side is a conjunction, and we're taking a disjunction.
            // Use the fact that OR distributes over AND to build a new conjunction:
            //     (a & b) | (x & y) == (a | x) & (a | y) & (b | x) & (b | y)
            PartialSchemaRequirements resultMap;
            for (const auto& [rightKey1, rightReq1] : rightReqMap) {
                for (const auto& [leftKey1, leftReq1] : leftReqMap) {
                    auto combinedIntervals = leftReq1.getIntervals();
                    combineIntervalsDNF(
                        false /*intersect*/, combinedIntervals, rightReq1.getIntervals());

                    PartialSchemaRequirement combinedReq{
                        // We already asserted that there are no projections.
                        boost::none,
                        std::move(combinedIntervals),
                        leftReq1.getIsPerfOnly(),
                    };
                    resultMap.emplace(leftKey1, combinedReq);
                }
            }

            leftReqMap = std::move(resultMap);
            return leftResult;
        }

        if (leftReqMap.size() != 1 || rightReqMap.size() != 1) {
            return {};
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
        if (leftReq.getBoundProjectionName() || rightReq.getBoundProjectionName()) {
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

        // Create new interval which uses the first element of the array, or "undefined" if the
        // interval is the empty array.
        ABT elementBound = arr->size() == 0
            ? make<Constant>(sbe::value::TypeTags::bsonUndefined, 0)
            : Constant::createFromCopy(arr->getAt(0).first, arr->getAt(0).second);
        const IntervalReqExpr::Node& newInterval =
            IntervalReqExpr::makeSingularDNF(IntervalRequirement{
                {true /*inclusive*/, elementBound}, {true /*inclusive*/, elementBound}});

        const ABT& leftPath = leftKey._path;
        const ABT& rightPath = rightKey._path;
        if (const auto leftTraversePtr = leftPath.cast<PathTraverse>();
            leftTraversePtr != nullptr && leftTraversePtr->getPath().is<PathIdentity>() &&
            rightPath.is<PathIdentity>()) {
            // If leftPath = Id and rightPath = Traverse Id, union the intervals, and introduce a
            // perf-only requirement.

            auto resultIntervals = leftIntervals;
            combineIntervalsDNF(false /*intersect*/, resultIntervals, newInterval);
            leftReq = {
                leftReq.getBoundProjectionName(), std::move(resultIntervals), true /*isPerfOnly*/};
            leftResult->_retainPredicate = true;
            return leftResult;
        } else if (const auto rightTraversePtr = rightPath.cast<PathTraverse>();
                   rightTraversePtr != nullptr && rightTraversePtr->getPath().is<PathIdentity>() &&
                   leftPath.is<PathIdentity>()) {
            // If leftPath = Traverse Id and rightPath = Id, union the intervals, and introduce a
            // perf-only requirement.

            auto resultIntervals = rightIntervals;
            combineIntervalsDNF(false /*intersect*/, resultIntervals, newInterval);
            rightReq = {
                rightReq.getBoundProjectionName(), std::move(resultIntervals), true /*isPerfOnly*/};
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

        return handleComposition<true /*isMultiplicative*/>(std::move(leftResult),
                                                            std::move(rightResult));
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
                {PartialSchemaKey{make<PathIdentity>()},
                 PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                          std::move(intervalExpr),
                                          false /*isPerfOnly*/}}}}};
        }

        return handleComposition<false /*isMultiplicative*/>(std::move(leftResult),
                                                             std::move(rightResult));
    }

    template <class T>
    static ResultType handleGetAndTraverse(const ABT& n, ResultType inputResult) {
        if (!inputResult) {
            return {};
        }
        if (inputResult->_bound) {
            return {};
        }

        // New map has keys with appended paths.
        PartialSchemaRequirements newMap;

        for (auto& entry : inputResult->_reqMap) {
            if (entry.first._projectionName) {
                return {};
            }

            ABT path = entry.first._path;

            // Updated key path to be now rooted at n, with existing key path as child.
            ABT appendedPath = n;
            std::swap(appendedPath.cast<T>()->getPath(), path);
            std::swap(path, appendedPath);

            newMap.emplace(PartialSchemaKey{std::move(path)}, std::move(entry.second));
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
            // More than one requirement means we are handling a conjunction inside a traverse.
            // We can change it to a traverse inside a conjunction, but that's an
            // over-approximation, so we have to keep the original predicate.
            inputResult->_retainPredicate = true;
        }

        auto result = handleGetAndTraverse<PathTraverse>(n, std::move(inputResult));
        if (result) {
            result->_hasTraversed = true;
        }
        return result;
    }

    /**
     * Convert to PathCompare EqMember to partial schema requirements if possible.
     */
    ResultType makeEqMemberInterval(const ABT& bound) {
        const auto boundConst = bound.cast<Constant>();
        if (boundConst == nullptr) {
            return {};
        }

        const auto [boundTag, boundVal] = boundConst->get();
        if (boundTag != sbe::value::TypeTags::Array) {
            return {};
        }
        const auto boundArray = sbe::value::getArrayView(boundVal);

        // Union the single intervals together. If we have PathCompare [EqMember] Const [[1, 2, 3]]
        // we create [1, 1] U [2, 2] U [3, 3].
        boost::optional<IntervalReqExpr::Node> unionedInterval;

        for (size_t i = 0; i < boundArray->size(); i++) {
            auto singleBoundLow =
                Constant::createFromCopy(boundArray->getAt(i).first, boundArray->getAt(i).second);
            auto singleBoundHigh = singleBoundLow;

            auto singleInterval = IntervalReqExpr::makeSingularDNF(
                IntervalRequirement{{true /*inclusive*/, std::move(singleBoundLow)},
                                    {true /*inclusive*/, std::move(singleBoundHigh)}});

            if (unionedInterval) {
                // Union the singleInterval with the unionedInterval we want to update.
                combineIntervalsDNF(false /*intersect*/, *unionedInterval, singleInterval);
            } else {
                unionedInterval = std::move(singleInterval);
            }
        }

        return {{PartialSchemaRequirements{
            {PartialSchemaKey{make<PathIdentity>()},
             PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                      std::move(*unionedInterval),
                                      false /*isPerfOnly*/}}}}};
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
            case Operations::EqMember:
                return makeEqMemberInterval(bound);

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
            {PartialSchemaKey{make<PathIdentity>()},
             PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                      std::move(intervalExpr),
                                      false /*isPerfOnly*/}}}}};
    }

    ResultType transport(const ABT& n, const PathIdentity& pathIdentity) {
        return {{PartialSchemaRequirements{{{n},
                                            {boost::none /*boundProjectionName*/,
                                             IntervalReqExpr::makeSingularDNF(),
                                             false /*isPerfOnly*/}}}}};
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

        if (_pathToInterval) {
            // If we have a path converter, attempt to convert directly into bounds.
            if (auto conversion = _pathToInterval(n); conversion) {
                return {{PartialSchemaRequirements{
                    {PartialSchemaKey{make<PathIdentity>()},
                     PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                              std::move(*conversion),
                                              false /*isPerfOnly*/}}}}};
            }
        }

        // General case. Reject conversion.
        return {};
    }

    ResultType convert(const ABT& input) {
        return algebra::transport<true>(input, *this);
    }

private:
    const bool _isFilterContext;
    const PathToIntervalFn& _pathToInterval;
};

boost::optional<PartialSchemaReqConversion> convertExprToPartialSchemaReq(
    const ABT& expr, const bool isFilterContext, const PathToIntervalFn& pathToInterval) {
    PartialSchemaReqConverter converter(isFilterContext, pathToInterval);
    auto result = converter.convert(expr);
    if (!result) {
        return {};
    }

    auto& reqMap = result->_reqMap;
    if (reqMap.empty()) {
        return {};
    }

    for (const auto& [key, req] : reqMap) {
        if (key._path.is<PathIdentity>() && isIntervalReqFullyOpenDNF(req.getIntervals())) {
            // We need to determine either path or interval (or both).
            return {};
        }
    }

    // If we over-approximate, we need to switch all requirements to perf-only.
    if (result->_retainPredicate) {
        for (auto& [key, req] : reqMap) {
            if (!req.getIsPerfOnly()) {
                req = {req.getBoundProjectionName(), req.getIntervals(), true /*isPerfOnly*/};
            }
        }
    }
    return result;
}

/**
 * Check if an index path contains a Traverse element.
 */
class PathTraverseChecker {
public:
    PathTraverseChecker() {}

    bool transport(const ABT& /*n*/, const PathTraverse& /*node*/, bool /*childResult*/) {
        return true;
    }

    bool transport(const ABT& /*n*/, const PathGet& /*node*/, bool childResult) {
        return childResult;
    }

    bool transport(const ABT& /*n*/, const PathIdentity& /*node*/) {
        return false;
    }

    template <typename T, typename... Ts>
    bool transport(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        uasserted(6624153, "Index paths only consist of Get, Traverse, and Id nodes.");
        return false;
    }

    bool check(const ABT& path) {
        return algebra::transport<true>(path, *this);
    }
};

bool checkPathContainsTraverse(const ABT& path) {
    return PathTraverseChecker{}.check(path);
}

/**
 * Removes Traverse nodes from a single path, using MultikeynessTrie which tells us
 * which child paths are never applied to an array.
 */
class MultikeynessSimplifier {
public:
    bool operator()(ABT&, PathIdentity&, const MultikeynessTrie&, bool /*skippedParentTraverse*/) {
        // No simplifications apply here.
        return false;
    }

    bool operator()(ABT& path,
                    PathGet& get,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        if (auto it = trie.children.find(get.name()); it != trie.children.end()) {
            return get.getPath().visit(*this, it->second, false /*skippedParentTraverse*/);
        } else {
            return false;
        }
    }

    bool operator()(ABT& path,
                    PathTraverse& traverse,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        tassert(6859603,
                "Unexpected maxDepth for Traverse in MultikeynessSimplifier",
                traverse.getMaxDepth() == PathTraverse::kSingleLevel);

        if (!trie.isMultiKey) {
            // This path is never applied to an array: we can remove any number of Traverse nodes,
            // of any maxDepth.
            path = std::exchange(traverse.getPath(), make<Blackhole>());
            // The parent can't have been a Traverse that we skipped, because we would have
            // removed it, because !trie.isMultiKey.
            invariant(!skippedParentTraverse);
            path.visit(*this, trie, false /*skippedParentTraverse*/);
            return true;
        } else if (traverse.getMaxDepth() == PathTraverse::kSingleLevel && !skippedParentTraverse) {
            // This path is possibly multikey, so we can't remove any Traverse nodes.
            // But each edge in the trie represents a 'Traverse [1] Get [a]', so we can
            // skip a single Traverse [1] node.
            return traverse.getPath().visit(*this, trie, true /*skippedParentTraverse*/);
        } else {
            // We have no information about multikeyness of the child path.
            return false;
        }
    }

    bool operator()(ABT& path,
                    PathLambda& pathLambda,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        // Look for PathLambda Lambda [tmp] UnaryOp [Not] EvalFilter <path> Variable [tmp],
        // and simplify <path>.  This works because 'tmp' is the same variable name in both places,
        // so <path> is applied to the same input as the PathLambda. (And the 'trie' tells us
        // which parts of that input are not arrays.)

        // In the future we may want to generalize this to skip over other expressions besides Not,
        // as long as the Lambda and EvalFilter are connected by a variable.

        if (auto* lambda = pathLambda.getLambda().cast<LambdaAbstraction>()) {
            if (auto* unary = lambda->getBody().cast<UnaryOp>();
                unary && unary->op() == Operations::Not) {
                if (auto* evalFilter = unary->getChild().cast<EvalFilter>()) {
                    if (auto* variable = evalFilter->getInput().cast<Variable>();
                        variable && variable->name() == lambda->varName()) {
                        return evalFilter->getPath().visit(
                            *this, trie, false /*skippedParentTraverse*/);
                    }
                }
            }
        }
        return false;
    }

    bool operator()(ABT& path,
                    PathComposeM& compose,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        const bool simplified1 = compose.getPath1().visit(*this, trie, skippedParentTraverse);
        const bool simplified2 = compose.getPath2().visit(*this, trie, skippedParentTraverse);
        return simplified1 || simplified2;
    }

    template <typename T, typename... Ts>
    bool operator()(ABT& n, T& /*node*/, Ts&&...) {
        // Don't optimize a node we don't recognize.
        return false;

        // Some other cases to consider:
        // - Remove PathArr for non-multikey paths.
        // - Descend into disjunction.
        // - Descend into PathLambda and simplify expressions, especially Not and EvalFilter.
    }

    static bool simplify(ABT& path, const MultikeynessTrie& trie) {
        MultikeynessSimplifier instance;
        return path.visit(instance, trie, false /*skippedParentTraverse*/);
    }
};

bool simplifyTraverseNonArray(ABT& path, const MultikeynessTrie& multikeynessTrie) {
    return MultikeynessSimplifier::simplify(path, multikeynessTrie);
}

bool simplifyPartialSchemaReqPaths(const ProjectionName& scanProjName,
                                   const MultikeynessTrie& multikeynessTrie,
                                   PartialSchemaRequirements& reqMap,
                                   const ConstFoldFn& constFold) {
    PartialSchemaRequirements result;
    boost::optional<std::pair<PartialSchemaKey, PartialSchemaRequirement>> prevEntry;

    const auto intersectFn = [&constFold](IntervalReqExpr::Node& intervals) {
        auto intersected = intersectDNFIntervals(intervals, constFold);
        if (!intersected) {
            // Empty interval.
            return false;
        }

        intervals = std::move(*intersected);
        return true;
    };

    // Simplify paths by eliminating unnecessary Traverse elements.
    for (const auto& [key, req] : reqMap) {
        PartialSchemaKey newKey = key;
        bool simplified = false;
        if (key._projectionName == scanProjName && checkPathContainsTraverse(newKey._path)) {
            simplified |= simplifyTraverseNonArray(newKey._path, multikeynessTrie);
        }

        if (prevEntry) {
            if (simplified && prevEntry->first == newKey) {
                auto& prevReq = prevEntry->second;

                boost::optional<ProjectionName> resultBoundProjName;
                auto resultIntervals = prevReq.getIntervals();
                if (const auto& boundProjName = req.getBoundProjectionName()) {
                    tassert(6624168,
                            "Should not be seeing more than one bound projection per key",
                            !prevReq.getBoundProjectionName());
                    resultBoundProjName = boundProjName;
                } else {
                    resultBoundProjName = prevReq.getBoundProjectionName();
                }

                combineIntervalsDNF(true /*intersect*/, resultIntervals, req.getIntervals());
                if (!intersectFn(resultIntervals)) {
                    return true;
                }

                prevReq = {std::move(resultBoundProjName),
                           std::move(resultIntervals),
                           req.getIsPerfOnly() && prevReq.getIsPerfOnly()};
            } else {
                result.insert(std::move(*prevEntry));
                prevEntry.reset({std::move(newKey), req});
            }
        } else {
            prevEntry.reset({std::move(newKey), req});
        }
    }
    if (prevEntry) {
        result.insert(std::move(*prevEntry));
    }

    // Intersect and normalize intervals.
    for (auto& [key, req] : result) {
        auto resultIntervals = req.getIntervals();
        if (!intersectFn(resultIntervals)) {
            return true;
        }

        normalizeIntervals(resultIntervals);

        req = {req.getBoundProjectionName(), std::move(resultIntervals), req.getIsPerfOnly()};
    }

    reqMap = std::move(result);
    return false;
}

/**
 * Try to compute the intersection of a an existing PartialSchemaRequirements object and a new
 * key/requirement pair.
 *
 * Returns false on "failure", which means the result was not representable for some reason.
 * Reasons include:
 * - The intersection predicate is always-false.
 * - There is a def-use dependency that combining into one PartialSchemaRequirements would break.
 *
 * The implementation works as follows:
 *     1. If the path of the new requirement does not exist in existing requirements, we add it.
 *     2. If the path already exists:
 *         2a. The path is multikey (e.g. Get "a" Traverse Id):
 *             Add the new requirement to the requirements (under the same key)
 *         2b. The path is not multikey (e.g. Get "a" Id):
 *             If we have an entry with the same path, combine intervals,
 *             otherwise add to requirements.
 *     3. We have an entry which binds the variable which the requirement uses.
 *        Append the paths (to rephrase the new requirement in terms of bindings
 *        visible in the input of the existing requirements) and retry. We will either
 *        add a new requirement or combine with an existing one.
 */
static bool intersectPartialSchemaReq(PartialSchemaRequirements& reqMap,
                                      PartialSchemaKey key,
                                      PartialSchemaRequirement req,
                                      ProjectionRenames& projectionRenames) {
    for (;;) {
        bool merged = false;

        const bool pathIsId = key._path.is<PathIdentity>();
        const bool pathHasTraverse = !pathIsId && checkPathContainsTraverse(key._path);
        {
            bool success = false;
            // Look for exact match on the path, and if found combine intervals.
            for (auto& [existingKey, existingReq] : reqMap) {
                if (existingKey != key) {
                    continue;
                }

                auto resultIntervals = existingReq.getIntervals();
                if (pathHasTraverse) {
                    combineIntervalsDNF(true /*intersect*/, resultIntervals, req.getIntervals());
                    if (resultIntervals == existingReq.getIntervals()) {
                        // Existing interval subsumes the new one.
                        success = true;
                    }
                } else {
                    // Non-multikey path. Directly intersect and simplify intervals.
                    combineIntervalsDNF(true /*intersect*/, resultIntervals, req.getIntervals());
                    success = true;
                }

                if (success) {
                    boost::optional<ProjectionName> resultBoundProjName =
                        existingReq.getBoundProjectionName();
                    if (const auto& boundProjName = req.getBoundProjectionName()) {
                        if (resultBoundProjName) {
                            // The new and existing projections both bind a name, so:
                            // - The existing name wins (stays in 'reqMap').
                            // - We tell the caller that the name bound is 'req' is now
                            //   available by the name in 'existingReq'.
                            projectionRenames.emplace(*boundProjName, *resultBoundProjName);
                        } else {
                            // Only the new projection binds a name, so we'll update 'reqMap' to
                            // include it.
                            resultBoundProjName = boundProjName;
                        }
                    }

                    existingReq = {std::move(resultBoundProjName),
                                   std::move(resultIntervals),
                                   req.getIsPerfOnly() && existingReq.getIsPerfOnly()};

                    // No need to iterate further: we could continue, but the result would not
                    // change.
                    break;
                }
            }

            if (success) {
                return true;
            }
        }

        const bool reqHasBoundProj = req.getBoundProjectionName().has_value();
        for (const auto& [existingKey, existingReq] : reqMap) {
            uassert(6624150,
                    "Existing key referring to new requirement.",
                    !reqHasBoundProj ||
                        existingKey._projectionName != *req.getBoundProjectionName());

            if (const auto& boundProjName = existingReq.getBoundProjectionName();
                boundProjName && key._projectionName == *boundProjName) {
                // The new key is referring to a projection the existing requirement binds.
                if (reqHasBoundProj) {
                    return false;
                }

                key = PartialSchemaKey{
                    existingKey._projectionName,
                    PathAppender::append(existingKey._path, std::move(key._path)),
                };
                merged = true;
                break;
            }
        }

        if (merged) {
            // continue around the loop
        } else {
            reqMap.emplace(std::move(key), std::move(req));
            return true;
        }
    }
    MONGO_UNREACHABLE;
}

bool isSubsetOfPartialSchemaReq(const PartialSchemaRequirements& lhs,
                                const PartialSchemaRequirements& rhs) {
    // We can use set intersection to calculate set containment:
    //   lhs <= rhs  iff  (lhs ^ rhs) == lhs

    // However, we're assuming that 'rhs' has no projections.
    // If it did have projections, the result (lhs ^ rhs) would have projections
    // and wouldn't match 'lhs'.
    for (auto&& [key, req] : rhs) {
        tassert(7155010,
                "isSubsetOfPartialSchemaReq expects 'rhs' to have no projections",
                !req.getBoundProjectionName());
    }

    ProjectionRenames projectionRenames_unused;
    PartialSchemaRequirements intersection = lhs;
    if (intersectPartialSchemaReq(intersection, rhs, projectionRenames_unused)) {
        return intersection == lhs;
    }
    // Intersection was empty-set, and we assume neither input is empty-set.
    // So intersection != lhs.
    return false;
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

const ProjectionName& getExistingOrTempProjForFieldName(PrefixId& prefixId,
                                                        const FieldNameType& fieldName,
                                                        FieldProjectionMap& fieldProjMap) {
    auto it = fieldProjMap._fieldProjections.find(fieldName);
    if (it != fieldProjMap._fieldProjections.cend()) {
        return it->second;
    }

    ProjectionName tempProjName = prefixId.getNextId("evalTemp");
    const auto result = fieldProjMap._fieldProjections.emplace(fieldName, std::move(tempProjName));
    invariant(result.second);
    return result.first->second;
}

/**
 * Fuses an index path and a query path to determine a residual path to apply over the index
 * results. Checks if one index path is a prefix of another. Considers only Get, Traverse, and Id.
 * Return the suffix that doesn't match.
 */
class IndexPathFusor {
public:
    struct ResultType {
        boost::optional<ABT::reference_type> _suffix;
        size_t _numTraversesSkipped = 0;
        size_t _numTraversesFused = 0;
    };

    /**
     * 'n' - The complete index path being compared to, can be modified if needed.
     * 'node' - Same as 'n' but cast to a specific type by the caller in order to invoke the
     *   correct operator.
     * 'other' - The query path, of which the index may satisfy a prefix.
     */
    ResultType operator()(const ABT& n, const PathGet& node, const ABT& other) {
        if (auto otherGet = other.cast<PathGet>();
            otherGet != nullptr && otherGet->name() == node.name()) {
            if (auto otherChildTraverse = otherGet->getPath().cast<PathTraverse>();
                otherChildTraverse != nullptr && !node.getPath().is<PathTraverse>()) {
                // If a query path has a Traverse, but the index path doesn't, the query can
                // still be evaluated by this index. Skip the Traverse node, and continue matching.
                // This works because we know the Traverse will never be applied to an array,
                // so 'Traverse [anything] p == p'.
                auto result = node.getPath().visit(*this, otherChildTraverse->getPath());
                result._numTraversesSkipped++;
                return result;
            } else {
                return node.getPath().visit(*this, otherGet->getPath());
            }
        }
        return {};
    }

    ResultType operator()(const ABT& n, const PathTraverse& node, const ABT& other) {
        if (auto otherTraverse = other.cast<PathTraverse>();
            otherTraverse != nullptr && otherTraverse->getMaxDepth() == node.getMaxDepth()) {
            auto result = node.getPath().visit(*this, otherTraverse->getPath());
            result._numTraversesFused++;
            return result;
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

    static ResultType fuse(const ABT& node, const ABT& candidatePrefix) {
        IndexPathFusor instance;
        return candidatePrefix.visit(instance, node);
    }
};

/**
 * Pad compound interval with supplied simple intervals
 */
static void padCompoundInterval(const IndexCollationSpec& indexCollationSpec,
                                CompoundIntervalReqExpr::Node& expr,
                                const size_t startIndex,
                                std::vector<IntervalReqExpr::Node> intervals) {
    for (size_t i = startIndex; i < startIndex + intervals.size(); i++) {
        const bool reverse = indexCollationSpec.at(i)._op == CollationOp::Descending;
        if (!combineCompoundIntervalsDNF(expr, std::move(intervals.at(i - startIndex)), reverse)) {
            uasserted(6624159, "Cannot combine with an open interval");
        }
    }
}

/**
 * Attempts to extend the compound interval corresponding to the last equality prefix to encode the
 * supplied required interval. If the equality prefix cannot be extended we begin a new equality
 * prefix and instead it instead. In the return value we indicate if we have exceeded the maximum
 * number of allowed equality prefixes.
 */
static bool extendCompoundInterval(PrefixId& prefixId,
                                   const IndexCollationSpec& indexCollationSpec,
                                   const size_t maxIndexEqPrefixes,
                                   const size_t indexField,
                                   const bool reverse,
                                   const IntervalReqExpr::Node& requiredInterval,
                                   std::vector<EqualityPrefixEntry>& eqPrefixes,
                                   ProjectionNameOrderPreservingSet& correlatedProjNames,
                                   FieldProjectionMap& fieldProjMap) {
    while (!combineCompoundIntervalsDNF(eqPrefixes.back()._interval, requiredInterval, reverse)) {
        // Should exit after at most one iteration.

        // Pad old prefix with open intervals to the end.
        padCompoundInterval(
            indexCollationSpec,
            eqPrefixes.back()._interval,
            indexField,
            {indexCollationSpec.size() - indexField, IntervalReqExpr::makeSingularDNF()});

        if (eqPrefixes.size() < maxIndexEqPrefixes) {
            // Begin new equality prefix.
            eqPrefixes.emplace_back(indexField);
            // Pad new prefix with index fields processed so far.

            for (size_t i = 0; i < indexField; i++) {
                // Obtain existing bound projection or get a temporary one for the
                // previous fields.
                const ProjectionName& tempProjName = getExistingOrTempProjForFieldName(
                    prefixId, FieldNameType{encodeIndexKeyName(i)}, fieldProjMap);
                correlatedProjNames.emplace_back(tempProjName);

                // Create point bounds using the projections.
                const BoundRequirement eqBound{true /*inclusive*/, make<Variable>(tempProjName)};
                padCompoundInterval(indexCollationSpec,
                                    eqPrefixes.back()._interval,
                                    i,
                                    {IntervalReqExpr::makeSingularDNF(eqBound, eqBound)});
            }
        } else {
            // Too many equality prefixes.
            return false;
        }
    }

    return true;
}

static bool computeCandidateIndexEntry(PrefixId& prefixId,
                                       const PartialSchemaRequirements& reqMap,
                                       const size_t maxIndexEqPrefixes,
                                       PartialSchemaKeySet unsatisfiedKeys,
                                       const ProjectionName& scanProjName,
                                       const bool fastNullHandling,
                                       const ConstFoldFn& constFold,
                                       const IndexCollationSpec& indexCollationSpec,
                                       CandidateIndexEntry& entry) {
    auto& fieldProjMap = entry._fieldProjectionMap;
    auto& eqPrefixes = entry._eqPrefixes;
    auto& correlatedProjNames = entry._correlatedProjNames;

    // Don't allow more than one Traverse predicate to fuse with the same Traverse in the index. For
    // each field of the index, track whether we are still allowed to fuse a Traverse predicate with
    // it.
    std::vector<bool> allowFuseTraverse(indexCollationSpec.size(), true);

    // Add open interval for the first equality prefix.
    eqPrefixes.emplace_back(0);

    for (size_t indexField = 0; indexField < indexCollationSpec.size(); indexField++) {
        const auto& indexCollationEntry = indexCollationSpec.at(indexField);
        const bool reverse = indexCollationEntry._op == CollationOp::Descending;
        bool foundSuitableField = false;

        PartialSchemaKey indexKey{scanProjName, indexCollationEntry._path};
        if (auto indexKeyIt = reqMap.find(indexKey); indexKeyIt != reqMap.cend()) {
            if (const PartialSchemaRequirement& req = indexKeyIt->second;
                fastNullHandling || !req.getIsPerfOnly() || !req.mayReturnNull(constFold)) {
                const auto& requiredInterval = req.getIntervals();
                const bool success = extendCompoundInterval(prefixId,
                                                            indexCollationSpec,
                                                            maxIndexEqPrefixes,
                                                            indexField,
                                                            reverse,
                                                            requiredInterval,
                                                            eqPrefixes,
                                                            correlatedProjNames,
                                                            fieldProjMap);
                if (!success) {
                    // Too many equality prefixes. Attempt to satisfy remaining predicates as
                    // residual.
                    break;
                }
                if (eqPrefixes.size() > 1 && entry._intervalPrefixSize == 0) {
                    // Need to constrain at least one field per prefix.
                    return false;
                }

                foundSuitableField = true;
                unsatisfiedKeys.erase(indexKey);
                if (checkPathContainsTraverse(indexKey._path)) {
                    allowFuseTraverse[indexField] = false;
                }
                entry._intervalPrefixSize++;

                const size_t queryPredPos = std::distance(reqMap.cbegin(), indexKeyIt);
                eqPrefixes.back()._predPosSet.insert(queryPredPos);

                if (const auto& boundProjName = req.getBoundProjectionName()) {
                    // Include bounds projection into index spec.
                    const bool inserted =
                        fieldProjMap._fieldProjections
                            .emplace(encodeIndexKeyName(indexField), *boundProjName)
                            .second;
                    invariant(inserted);
                }

                if (auto singularInterval = IntervalReqExpr::getSingularDNF(requiredInterval);
                    !singularInterval || !singularInterval->isEquality()) {
                    // We only care about collation of for non-equality intervals.
                    // Equivalently, it is sufficient for singular intervals to be clustered.
                    entry._fieldsToCollate.insert(indexField);
                }
            }
        }

        if (!foundSuitableField) {
            // We cannot constrain the current index field.
            padCompoundInterval(indexCollationSpec,
                                eqPrefixes.back()._interval,
                                indexField,
                                {IntervalReqExpr::makeSingularDNF()});
        }
    }

    // Compute residual predicates from unsatisfied partial schema keys.
    for (auto queryKeyIt = unsatisfiedKeys.begin(); queryKeyIt != unsatisfiedKeys.end();) {
        const auto& queryKey = *queryKeyIt;
        bool satisfied = false;

        for (size_t indexField = 0; indexField < indexCollationSpec.size(); indexField++) {
            if (const auto fusedPath =
                    IndexPathFusor::fuse(queryKey._path, indexCollationSpec.at(indexField)._path);
                fusedPath._suffix &&
                (allowFuseTraverse[indexField] || fusedPath._numTraversesFused == 0)) {
                if (fusedPath._numTraversesFused > 0) {
                    allowFuseTraverse[indexField] = false;
                }
                auto it = reqMap.find(queryKey);
                tassert(
                    6624158, "QueryKey must exist in the requirements map", it != reqMap.cend());

                const PartialSchemaRequirement& req = it->second;
                if (!req.getIsPerfOnly()) {
                    // Only regular requirements are added to residual predicates.
                    const ProjectionName& tempProjName = getExistingOrTempProjForFieldName(
                        prefixId, FieldNameType{encodeIndexKeyName(indexField)}, fieldProjMap);
                    entry._residualRequirements.emplace_back(
                        PartialSchemaKey{tempProjName, std::move(*fusedPath._suffix)},
                        req,
                        std::distance(reqMap.cbegin(), it));
                }

                satisfied = true;
                break;
            }
        }

        if (satisfied) {
            unsatisfiedKeys.erase(queryKeyIt++);
        } else {
            // We have found at least one entry which we cannot satisfy. We can only encode as
            // residual predicates which use as input value obtained from an index field. If the
            // predicate refers to a field not in the index, then we cannot satisfy here. Presumably
            // the predicate would be moved to the seek side in a prior logical rewrite which would
            // split the original sargable nodes into two.
            break;
        }
    }

    if (!unsatisfiedKeys.empty()) {
        // Could not satisfy all query requirements. Note at this point may contain entries which
        // can actually be satisfied but were not attempted to be satisfied as we exited the
        // residual predicate loop on the first failure.
        return false;
    }
    if (entry._intervalPrefixSize == 0 && entry._residualRequirements.empty()) {
        // Need to encode at least one query requirement in the index bounds.
        return false;
    }

    return true;
}

CandidateIndexes computeCandidateIndexes(PrefixId& prefixId,
                                         const ProjectionName& scanProjectionName,
                                         const PartialSchemaRequirements& reqMap,
                                         const ScanDefinition& scanDef,
                                         const QueryHints& hints,
                                         bool& hasEmptyInterval,
                                         const ConstFoldFn& constFold) {
    // Contains one instance for each unmatched key.
    PartialSchemaKeySet unsatisfiedKeysInitial;
    for (const auto& [key, req] : reqMap) {
        if (req.getIsPerfOnly()) {
            // Perf only do not need to be necessarily satisfied.
            continue;
        }

        if (!unsatisfiedKeysInitial.insert(key).second) {
            // We cannot satisfy two or more non-multikey path instances using an index.
            return {};
        }

        if (!hints._fastIndexNullHandling && !req.getIsPerfOnly() && req.mayReturnNull(constFold)) {
            // We cannot use indexes to return values for fields if we have an interval with null
            // bounds.
            return {};
        }
    }

    CandidateIndexes result;
    for (const auto& [indexDefName, indexDef] : scanDef.getIndexDefs()) {
        for (size_t i = hints._minIndexEqPrefixes; i <= hints._maxIndexEqPrefixes; i++) {
            CandidateIndexEntry entry(indexDefName);
            const bool success = computeCandidateIndexEntry(prefixId,
                                                            reqMap,
                                                            i,
                                                            unsatisfiedKeysInitial,
                                                            scanProjectionName,
                                                            hints._fastIndexNullHandling,
                                                            constFold,
                                                            indexDef.getCollationSpec(),
                                                            entry);

            if (success && entry._eqPrefixes.size() >= hints._minIndexEqPrefixes) {
                result.push_back(std::move(entry));
            }
        }
    }

    return result;
}

boost::optional<ScanParams> computeScanParams(PrefixId& prefixId,
                                              const PartialSchemaRequirements& reqMap,
                                              const ProjectionName& rootProj) {
    ScanParams result;
    auto& residualReqs = result._residualRequirements;
    auto& fieldProjMap = result._fieldProjectionMap;

    size_t entryIndex = 0;
    for (const auto& [key, req] : reqMap) {
        if (req.getIsPerfOnly()) {
            // Ignore perf only requirements.
            continue;
        }
        if (key._projectionName != rootProj) {
            // We are not sitting right above a ScanNode.
            return {};
        }

        if (auto pathGet = key._path.cast<PathGet>(); pathGet != nullptr) {
            const FieldNameType& fieldName = pathGet->name();

            // Extract a new requirements path with removed simple paths.
            // For example if we have a key Get "a" Traverse Compare = 0 we leave only
            // Traverse Compare 0.
            if (const auto& boundProjName = req.getBoundProjectionName();
                boundProjName && pathGet->getPath().is<PathIdentity>()) {
                const auto [it, insertedInFPM] =
                    fieldProjMap._fieldProjections.emplace(fieldName, *boundProjName);

                if (!insertedInFPM) {
                    residualReqs.emplace_back(PartialSchemaKey{it->second, make<PathIdentity>()},
                                              PartialSchemaRequirement{req.getBoundProjectionName(),
                                                                       req.getIntervals(),
                                                                       false /*isPerfOnly*/},
                                              entryIndex);
                } else if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                    residualReqs.emplace_back(
                        PartialSchemaKey{*boundProjName, make<PathIdentity>()},
                        PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                                 req.getIntervals(),
                                                 false /*isPerfOnly*/},
                        entryIndex);
                }
            } else {
                const ProjectionName& tempProjName =
                    getExistingOrTempProjForFieldName(prefixId, fieldName, fieldProjMap);
                residualReqs.emplace_back(
                    PartialSchemaKey{tempProjName, pathGet->getPath()}, req, entryIndex);
            }
        } else {
            // Move other conditions into the residual map.
            fieldProjMap._rootProjection = rootProj;
            residualReqs.emplace_back(key, req, entryIndex);
        }

        entryIndex++;
    }

    return result;
}

/**
 * Transport that checks if we have a primitive interval which may contain null.
 */
class PartialSchemaReqMayContainNullTransport {
public:
    bool transport(const IntervalReqExpr::Atom& node, const ConstFoldFn& constFold) {
        const auto& interval = node.getExpr();

        const auto foldFn = [&constFold](ABT expr) {
            constFold(expr);
            return expr;
        };
        if (const auto& lowBound = interval.getLowBound();
            foldFn(make<BinaryOp>(lowBound.isInclusive() ? Operations::Gt : Operations::Gte,
                                  lowBound.getBound(),
                                  Constant::null())) == Constant::boolean(true)) {
            // Lower bound is strictly larger than null, or equal to null but not inclusive.
            return false;
        }
        if (const auto& highBound = interval.getHighBound();
            foldFn(make<BinaryOp>(highBound.isInclusive() ? Operations::Lt : Operations::Lte,
                                  highBound.getBound(),
                                  Constant::null())) == Constant::boolean(true)) {
            // Upper bound is strictly smaller than null, or equal to null but not inclusive.
            return false;
        }

        return true;
    }

    bool transport(const IntervalReqExpr::Conjunction& node,
                   const ConstFoldFn& constFold,
                   std::vector<bool> childResults) {
        return std::all_of(
            childResults.cbegin(), childResults.cend(), [](const bool v) { return v; });
    }

    bool transport(const IntervalReqExpr::Disjunction& node,
                   const ConstFoldFn& constFold,
                   std::vector<bool> childResults) {
        return std::any_of(
            childResults.cbegin(), childResults.cend(), [](const bool v) { return v; });
    }

    bool check(const IntervalReqExpr::Node& intervals, const ConstFoldFn& constFold) {
        return algebra::transport<false>(intervals, *this, constFold);
    }
};

bool checkMaybeHasNull(const IntervalReqExpr::Node& intervals, const ConstFoldFn& constFold) {
    return PartialSchemaReqMayContainNullTransport{}.check(intervals, constFold);
}

class PartialSchemaReqLowerTransport {
public:
    PartialSchemaReqLowerTransport(const bool hasBoundProjName,
                                   const PathToIntervalFn& pathToInterval)
        : _hasBoundProjName(hasBoundProjName),
          _intervalArr(getInterval(pathToInterval, make<PathArr>())),
          _intervalObj(getInterval(pathToInterval, make<PathObj>())) {}

    ABT transport(const IntervalReqExpr::Atom& node) {
        const auto& interval = node.getExpr();
        const auto& lowBound = interval.getLowBound();
        const auto& highBound = interval.getHighBound();

        if (interval.isEquality()) {
            if (auto constPtr = lowBound.getBound().cast<Constant>()) {
                if (constPtr->isNull()) {
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

        if (interval == _intervalArr) {
            return make<PathArr>();
        }
        if (interval == _intervalObj) {
            return make<PathObj>();
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

    ABT transport(const IntervalReqExpr::Conjunction& node, ABTVector childResults) {
        // Construct a balanced multiplicative composition tree.
        maybeComposePaths<PathComposeM>(childResults);
        return std::move(childResults.front());
    }

    ABT transport(const IntervalReqExpr::Disjunction& node, ABTVector childResults) {
        // Construct a balanced additive composition tree.
        maybeComposePaths<PathComposeA>(childResults);
        return std::move(childResults.front());
    }

    ABT lower(const IntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }

private:
    /**
     * Convenience for looking up the singular interval that corresponds to either PathArr or
     * PathObj.
     */
    static boost::optional<IntervalRequirement> getInterval(const PathToIntervalFn& pathToInterval,
                                                            const ABT& path) {
        if (pathToInterval) {
            if (auto intervalExpr = pathToInterval(path)) {
                if (auto interval = IntervalReqExpr::getSingularDNF(*intervalExpr)) {
                    return *interval;
                }
            }
        }
        return boost::none;
    }

    const bool _hasBoundProjName;
    const boost::optional<IntervalRequirement> _intervalArr;
    const boost::optional<IntervalRequirement> _intervalObj;
};

void lowerPartialSchemaRequirement(const PartialSchemaKey& key,
                                   const PartialSchemaRequirement& req,
                                   ABT& node,
                                   const PathToIntervalFn& pathToInterval,
                                   const std::function<void(const ABT& node)>& visitor) {
    PartialSchemaReqLowerTransport transport(req.getBoundProjectionName().has_value(),
                                             pathToInterval);
    ABT path = transport.lower(req.getIntervals());
    const bool pathIsId = path.is<PathIdentity>();

    if (const auto& boundProjName = req.getBoundProjectionName()) {
        node = make<EvaluationNode>(*boundProjName,
                                    make<EvalPath>(key._path, make<Variable>(*key._projectionName)),
                                    std::move(node));
        visitor(node);

        if (!pathIsId) {
            node = make<FilterNode>(
                make<EvalFilter>(std::move(path), make<Variable>(*boundProjName)), std::move(node));
            visitor(node);
        }
    } else {
        uassert(
            6624162, "If we do not have a bound projection, then we have a proper path", !pathIsId);

        path = PathAppender::append(key._path, std::move(path));

        node = make<FilterNode>(
            make<EvalFilter>(std::move(path), make<Variable>(*key._projectionName)),
            std::move(node));
        visitor(node);
    }
}

void lowerPartialSchemaRequirements(const CEType scanGroupCE,
                                    std::vector<SelectivityType> indexPredSels,
                                    ResidualRequirementsWithCE& requirements,
                                    ABT& physNode,
                                    const PathToIntervalFn& pathToInterval,
                                    NodeCEMap& nodeCEMap) {
    sortResidualRequirements(requirements);

    for (const auto& [residualKey, residualReq, ce] : requirements) {
        CEType residualCE = scanGroupCE;
        if (!indexPredSels.empty()) {
            // We are intentionally making a copy of the vector here, we are adding elements to it
            // below.
            residualCE *= ce::conjExponentialBackoff(indexPredSels);
        }
        if (scanGroupCE > 0.0) {
            // Compute the selectivity after we assign CE, which is the "input" to the cost.
            indexPredSels.push_back(ce / scanGroupCE);
        }

        lowerPartialSchemaRequirement(
            residualKey, residualReq, physNode, pathToInterval, [&](const ABT& node) {
                nodeCEMap.emplace(node.cast<Node>(), residualCE);
            });
    }
}

void sortResidualRequirements(ResidualRequirementsWithCE& residualReq) {
    // Sort residual requirements by estimated cost.
    // Assume it is more expensive to deliver a bound projection than to just filter.

    std::vector<std::pair<double, size_t>> costs;
    for (size_t index = 0; index < residualReq.size(); index++) {
        const auto& entry = residualReq.at(index);

        size_t multiplier = 0;
        if (entry._req.getBoundProjectionName()) {
            multiplier++;
        }
        if (!isIntervalReqFullyOpenDNF(entry._req.getIntervals())) {
            multiplier++;
        }

        costs.emplace_back(entry._ce._value * multiplier, index);
    }

    std::sort(costs.begin(), costs.end());
    for (size_t index = 0; index < residualReq.size(); index++) {
        const size_t targetIndex = costs.at(index).second;
        if (index < targetIndex) {
            std::swap(residualReq.at(index), residualReq.at(targetIndex));
        }
    }
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

void removeRedundantResidualPredicates(const ProjectionNameOrderPreservingSet& requiredProjections,
                                       const ProjectionNameOrderPreservingSet& correlatedProjNames,
                                       ResidualRequirements& residualReqs,
                                       FieldProjectionMap& fieldProjectionMap) {
    ProjectionNameSet residualTempProjections;

    // Remove unused residual requirements.
    for (auto it = residualReqs.begin(); it != residualReqs.end();) {
        auto& [key, req, ce] = *it;

        if (const auto& boundProjName = req.getBoundProjectionName();
            boundProjName && !requiredProjections.find(*boundProjName)) {
            if (isIntervalReqFullyOpenDNF(req.getIntervals())) {
                residualReqs.erase(it++);
                continue;
            } else {
                // We do not use the output binding, but we still want to filter.
                tassert(6624163,
                        "Should not be seeing a perf-only predicate as residual",
                        !req.getIsPerfOnly());
                req = {boost::none /*boundProjectionName*/,
                       std::move(req.getIntervals()),
                       req.getIsPerfOnly()};
            }
        }

        residualTempProjections.insert(*key._projectionName);
        it++;
    }

    // Remove unused projections from the field projection map.
    auto& fieldProjMap = fieldProjectionMap._fieldProjections;
    for (auto it = fieldProjMap.begin(); it != fieldProjMap.end();) {
        if (const ProjectionName& projName = it->second; !requiredProjections.find(projName) &&
            residualTempProjections.count(projName) == 0 && !correlatedProjNames.find(projName)) {
            fieldProjMap.erase(it++);
        } else {
            it++;
        }
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

        if (leftProjections.find(projectionName)) {
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
        const SelectivityType indexSel =
            (scanGroupCE == 0.0) ? SelectivityType{0.0} : (indexCE / _scanGroupCE);
        _estimateStack.push_back(indexSel);
        _fpmStack.push_back(std::move(indexProjectionMap));
    };

    ABT transport(const CompoundIntervalReqExpr::Atom& node) {
        ABT physicalIndexScan = make<IndexScanNode>(
            _fpmStack.back(), _scanDefName, _indexDefName, node.getExpr(), _reverseOrder);
        _nodeCEMap.emplace(physicalIndexScan.cast<Node>(), _scanGroupCE * _estimateStack.back());
        return physicalIndexScan;
    }

    template <bool isConjunction>
    void prepare(const size_t childCount) {
        SelectivityType childSel{1.0};
        if (childCount > 0) {
            // Here we are assuming that children in each conjunction and disjunction contribute
            // equally and independently to the parent's selectivity.
            // TODO: consider estimates per individual interval.

            const SelectivityType parentSel = _estimateStack.back();
            const double childCountInv = 1.0 / childCount;
            if constexpr (isConjunction) {
                childSel = {(parentSel == 0.0) ? SelectivityType{0.0}
                                               : parentSel.pow(childCountInv)};
            } else {
                childSel = parentSel * childCountInv;
            }
        }
        _estimateStack.push_back(childSel);

        FieldProjectionMap childMap = _fpmStack.back();
        if (childCount > 1) {
            if (!childMap._ridProjection) {
                childMap._ridProjection = _ridProjName;
            }
            for (auto& [fieldName, projectionName] : childMap._fieldProjections) {
                projectionName = _prefixId.getNextId(isConjunction ? "conjunction" : "disjunction");
            }
        }
        _fpmStack.push_back(std::move(childMap));
    }

    void prepare(const CompoundIntervalReqExpr::Conjunction& node) {
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
        unionProjectionNames.push_back(_ridProjName);
        for (const auto& [fieldName, projectionName] : innerMap._fieldProjections) {
            unionProjectionNames.push_back(projectionName);
        }

        ProjectionNameVector outerProjNames;
        for (const auto& [fieldName, projectionName] : outerMap._fieldProjections) {
            outerProjNames.push_back(projectionName);
        }
        ProjectionNameVector aggProjectionNames = outerProjNames;

        ABTVector aggExpressions;
        for (const auto& [fieldName, projectionName] : innerMap._fieldProjections) {
            aggExpressions.emplace_back(
                make<FunctionCall>("$first", makeSeq(make<Variable>(projectionName))));
        }

        boost::optional<ProjectionName> sideSetProjectionName;
        if constexpr (isIntersect) {
            const ProjectionName sideIdProjectionName = _prefixId.getNextId("sideId");
            unionProjectionNames.push_back(sideIdProjectionName);
            sideSetProjectionName = _prefixId.getNextId("sides");

            for (size_t index = 0; index < inputSize; index++) {
                ABT& input = inputs.at(index);
                input = make<EvaluationNode>(
                    sideIdProjectionName, Constant::int64(index), std::move(input));
                // Not relevant for cost.
                _nodeCEMap.emplace(input.cast<Node>(), CEType{0.0});
            }

            aggExpressions.emplace_back(
                make<FunctionCall>("$addToSet", makeSeq(make<Variable>(sideIdProjectionName))));
            aggProjectionNames.push_back(*sideSetProjectionName);
        }

        ABT result = make<UnionNode>(std::move(unionProjectionNames), std::move(inputs));
        _nodeCEMap.emplace(result.cast<Node>(), ce);

        result = make<GroupByNode>(ProjectionNameVector{_ridProjName},
                                   std::move(aggProjectionNames),
                                   std::move(aggExpressions),
                                   std::move(result));
        _nodeCEMap.emplace(result.cast<Node>(), ce);

        if constexpr (isIntersect) {
            result = make<FilterNode>(
                make<EvalFilter>(
                    make<PathCompare>(Operations::Eq, Constant::int64(inputSize)),
                    make<FunctionCall>("getArraySize",
                                       makeSeq(make<Variable>(*sideSetProjectionName)))),
                std::move(result));
            _nodeCEMap.emplace(result.cast<Node>(), ce);
        } else if (!outerMap._ridProjection && !outerProjNames.empty()) {
            // Prevent rid projection from leaking out if we do not require it, and also auxiliary
            // left and right side projections.
            result = make<UnionNode>(std::move(outerProjNames), makeSeq(std::move(result)));
            _nodeCEMap.emplace(result.cast<Node>(), ce);
        }

        return result;
    }

    ABT transport(const CompoundIntervalReqExpr::Conjunction& node, ABTVector childResults) {
        return implement<true /*isIntersect*/>(std::move(childResults));
    }

    void prepare(const CompoundIntervalReqExpr::Disjunction& node) {
        prepare<false /*isConjunction*/>(node.nodes().size());
    }

    ABT transport(const CompoundIntervalReqExpr::Disjunction& node, ABTVector childResults) {
        return implement<false /*isIntersect*/>(std::move(childResults));
    }

    ABT lower(const CompoundIntervalReqExpr::Node& intervals) {
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

static ABT lowerIntervals(PrefixId& prefixId,
                          const ProjectionName& ridProjName,
                          FieldProjectionMap indexProjectionMap,
                          const std::string& scanDefName,
                          const std::string& indexDefName,
                          const CompoundIntervalReqExpr::Node& intervals,
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

ABT lowerEqPrefixes(PrefixId& prefixId,
                    const ProjectionName& ridProjName,
                    FieldProjectionMap indexProjectionMap,
                    const std::string& scanDefName,
                    const std::string& indexDefName,
                    const std::vector<EqualityPrefixEntry>& eqPrefixes,
                    const std::vector<bool>& reverseOrder,
                    const ProjectionNameVector& correlatedProjNames,
                    const std::map<size_t, SelectivityType>& indexPredSelMap,
                    const CEType currentGroupCE,
                    const CEType scanGroupCE,
                    NodeCEMap& nodeCEMap) {
    boost::optional<ABT> result;

    for (size_t eqPrefixIndex = 0; eqPrefixIndex < eqPrefixes.size(); eqPrefixIndex++) {
        const auto& eqPrefix = eqPrefixes.at(eqPrefixIndex);
        const size_t startPos = eqPrefix._startPos;

        FieldProjectionMap eqPrefixMap;
        if (eqPrefixIndex == eqPrefixes.size() - 1) {
            // If this is the last equality prefix, use the input field projection map but remove
            // the prefix of correlated projections.

            eqPrefixMap = indexProjectionMap;
            for (size_t indexField = 0; indexField < startPos; indexField++) {
                eqPrefixMap._fieldProjections.erase(FieldNameType{encodeIndexKeyName(indexField)});
            }
        } else {
            // If this is not the last equality prefix, create a field projection map using the
            // prefix of the correlated projections only.

            const size_t nextStartPos = eqPrefixes.at(eqPrefixIndex + 1)._startPos;
            for (size_t indexField = startPos; indexField < nextStartPos; indexField++) {
                const FieldNameType indexKey{encodeIndexKeyName(indexField)};
                eqPrefixMap._fieldProjections.emplace(
                    indexKey, indexProjectionMap._fieldProjections.at(indexKey));
            }
        }

        // Collect estimates for predicates satisfied with the current equality prefix.
        // TODO: rationalize cardinality estimates: estimate number of unique groups.
        CEType indexCE = currentGroupCE;
        if (!eqPrefix._predPosSet.empty()) {
            std::vector<SelectivityType> currentSels;
            for (const size_t index : eqPrefix._predPosSet) {
                if (const auto it = indexPredSelMap.find(index); it != indexPredSelMap.cend()) {
                    currentSels.push_back(it->second);
                }
            }
            if (!currentSels.empty()) {
                indexCE = scanGroupCE * ce::conjExponentialBackoff(std::move(currentSels));
            }
        }

        // Convert the prefix's into a tree of intervals.
        ABT outer = lowerIntervals(prefixId,
                                   ridProjName,
                                   std::move(eqPrefixMap),
                                   scanDefName,
                                   indexDefName,
                                   eqPrefix._interval,
                                   reverseOrder.at(eqPrefixIndex),
                                   indexCE,
                                   scanGroupCE,
                                   nodeCEMap);

        if (result) {
            // Compute correlation parameters based on the previous equality prefix.

            ProjectionNameVector correlationVector;
            ProjectionNameSet correlationSet;
            for (size_t indexField = 0; indexField < startPos; indexField++) {
                const auto& correlatedProjName = correlatedProjNames.at(indexField);
                correlationVector.push_back(correlatedProjName);
                correlationSet.insert(correlatedProjName);
            }

            ABT inner = make<UniqueNode>(std::move(correlationVector), std::move(*result));
            nodeCEMap.emplace(inner.cast<Node>(), indexCE);

            // TODO: SERVER-70639. Use a spool node for RIN plans.
            outer = make<NestedLoopJoinNode>(JoinType::Inner,
                                             std::move(correlationSet),
                                             Constant::boolean(true),
                                             std::move(inner),
                                             std::move(outer));
            nodeCEMap.emplace(outer.cast<Node>(), indexCE);
        }

        result = std::move(outer);
    }

    return std::move(*result);
}

/**
 * Checks if a path ends in a Traverse + PathId.
 */
class PathEndsInTraverseId {
public:
    bool transport(const optimizer::PathTraverse& node, bool childResult) {
        return node.getPath().is<PathIdentity>() || childResult;
    }

    bool transport(const optimizer::PathGet& /*node*/, bool childResult) {
        return childResult;
    }

    bool transport(const optimizer::PathIdentity& /*node*/) {
        return false;
    }

    template <typename T, typename... Ts>
    bool transport(const T& node, Ts&&... /* args */) {
        uasserted(6749500, "Unexpected node in transport to check if path is $elemMatch.");
    }
};

bool pathEndsInTraverse(const optimizer::ABT& path) {
    PathEndsInTraverseId t;
    return optimizer::algebra::transport<false>(path, t);
}

bool hasProperIntervals(const PartialSchemaRequirements& reqMap) {
    // Compute if this node has any proper (not fully open) intervals.
    for (const auto& [key, req] : reqMap) {
        if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
            return true;
        }
    }
    return false;
}

}  // namespace mongo::optimizer
