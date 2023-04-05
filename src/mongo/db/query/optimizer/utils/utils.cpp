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
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/db/storage/storage_parameters_gen.h"


namespace mongo::optimizer {

ABT makeBalancedBooleanOpTree(Operations logicOp, std::vector<ABT> leaves) {
    auto builder = [=](ABT lhs, ABT rhs) {
        return make<BinaryOp>(logicOp, std::move(lhs), std::move(rhs));
    };
    return makeBalancedTreeImpl(builder, leaves, 0, leaves.size());
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

void restrictProjections(ProjectionNameVector projNames, const CEType ce, PhysPlanBuilder& input) {
    input.make<UnionNode>(ce, std::move(projNames), makeSeq(std::move(input._node)));
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

    /**
     * Handle EvalPath and EvalFilter nodes.
     */
    ResultType handleEvalContext(ResultType pathResult, ResultType inputResult) {
        // In a (Eval <path> <input>) expression, we expect the <path> to result only in a path and
        // intervals: no input binding or output binding. The input binding comes from <input>, and
        // we don't expect <input> to have any predicates or output bindings.
        if (!pathResult || !inputResult) {
            return {};
        }
        if (pathResult->_bound || !inputResult->_bound || !inputResult->_reqMap.isNoop()) {
            return {};
        }

        if (auto* inputVar = inputResult->_bound->cast<Variable>()) {
            // Every Atom in pathResult has an unknown input binding.
            // Fill it in with 'inputVar'.

            const ProjectionName& inputVarName = inputVar->name();
            PSRExpr::visitAnyShape(pathResult->_reqMap.getRoot(), [&](PartialSchemaEntry& entry) {
                tassert(
                    7453903,
                    "Expected PartialSchemaReqConversion for a path to have its input left blank",
                    !entry.first._projectionName);
                entry.first._projectionName = inputVarName;
            });
            return pathResult;
        }

        return {};
    }

    ResultType transport(const ABT& n,
                         const EvalPath& evalPath,
                         ResultType pathResult,
                         ResultType inputResult) {
        if (_isFilterContext) {
            // 'pathResult' was translated as if it appeared in EvalFilter; we can't use it.
            return {};
        }
        return handleEvalContext(std::move(pathResult), std::move(inputResult));
    }

    ResultType transport(const ABT& n,
                         const EvalFilter& evalFilter,
                         ResultType pathResult,
                         ResultType inputResult) {
        if (!_isFilterContext) {
            // 'pathResult' was translated as if it appeared in EvalPath; we can't use it.
            return {};
        }
        return handleEvalContext(std::move(pathResult), std::move(inputResult));
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
            if (!intersectPartialSchemaReq(leftReqMap, rightReqMap)) {
                return {};
            }

            ProjectionRenames renames_unused;
            const bool hasEmptyInterval =
                simplifyPartialSchemaReqPaths(boost::none /*scanProjName*/,
                                              {} /*multikeynessTrie*/,
                                              leftReqMap,
                                              renames_unused,
                                              {} /*constFold*/);
            tassert(6624168,
                    "Cannot detect empty intervals without providing a constant folder",
                    !hasEmptyInterval);

            leftResult->_hasIntersected = true;
            return leftResult;
        }
        // From this point on we only handle additive composition.

        // Check if the left and right requirements are all or none perf-only.
        size_t perfOnlyCount = 0;
        for (const auto* reqs : {&leftReqMap, &rightReqMap}) {
            PSRExpr::visitAnyShape(reqs->getRoot(), [&](const PartialSchemaEntry& e) {
                const auto& [key, req] = e;
                // Additive composition should only have predicates; no projections.
                tassert(7155021,
                        "Unexpected binding in ComposeA in PartialSchemaReqConverter",
                        !req.getBoundProjectionName());

                if (req.getIsPerfOnly()) {
                    perfOnlyCount++;
                }
            });
        }
        if (perfOnlyCount != 0 &&
            perfOnlyCount !=
                PSRExpr::numLeaves(leftReqMap.getRoot()) +
                    PSRExpr::numLeaves(rightReqMap.getRoot())) {
            // For now allow only predicates with the same perf-only flag.
            return {};
        }

        if (ResultType sameFieldDisj = createSameFieldDisjunction(leftResult, rightResult)) {
            return sameFieldDisj;
        }

        // Handle a general disjunction.
        auto result = PartialSchemaReqConversion{
            unionPartialSchemaReq(std::move(leftReqMap), std::move(rightReqMap))};
        if (leftResult->_retainPredicate || rightResult->_retainPredicate) {
            // If either argument is an over-approximation, then so is the result.
            result._retainPredicate = true;
        }
        return result;
    }

    /**
     * Given two predicates, form their disjunction if we can represent the result as a conjunction
     * of predicates on the same field. Otherwise return an empty optional.
     *
     * When this function returns a nonempty optional, it may modify or move from the arguments.
     * When it returns boost::none the arguments are unchanged.
     *
     * TODO SERVER-73827 Instead of handling these special cases, just construct a disjunction and
     * then simplify; and get rid of this function.
     */
    static ResultType createSameFieldDisjunction(ResultType& leftResult, ResultType& rightResult) {
        auto& leftReqMap = leftResult->_reqMap;
        auto& rightReqMap = rightResult->_reqMap;
        if (!PSRExpr::isSingletonDisjunction(leftReqMap.getRoot()) ||
            !PSRExpr::isSingletonDisjunction(rightReqMap.getRoot())) {
            return {};
        }

        auto leftEntries = leftReqMap.conjuncts();
        auto rightEntries = rightReqMap.conjuncts();

        auto leftEntry = leftEntries.begin();
        auto rightEntry = rightEntries.begin();
        auto& [leftKey, leftReq] = *leftEntry;
        auto& [rightKey, rightReq] = *rightEntry;

        // Do all reqs from both sides use the same key?
        bool allSameKey = true;
        for (const auto* reqs : {&leftReqMap, &rightReqMap}) {
            for (auto&& [k, req] : reqs->conjuncts()) {
                if (k != leftKey) {
                    allSameKey = false;
                    break;
                }
            }
        }
        if (allSameKey) {
            // All reqs from both sides use the same key (input binding + path).

            // Each side is a conjunction, and we're taking a disjunction.
            // Use the fact that OR distributes over AND to build a new conjunction:
            //     (a & b) | (x & y) == (a | x) & (a | y) & (b | x) & (b | y)
            PSRExpr::Builder resultReqs;
            resultReqs.pushDisj().pushConj();
            for (const auto& [rightKey1, rightReq1] : rightReqMap.conjuncts()) {
                for (const auto& [leftKey1, leftReq1] : leftReqMap.conjuncts()) {
                    auto combinedIntervals = leftReq1.getIntervals();
                    combineIntervalsDNF(
                        false /*intersect*/, combinedIntervals, rightReq1.getIntervals());

                    PartialSchemaRequirement combinedReq{
                        // We already asserted that there are no projections.
                        boost::none,
                        std::move(combinedIntervals),
                        leftReq1.getIsPerfOnly(),
                    };
                    resultReqs.atom(leftKey1, combinedReq);
                }
            }

            leftReqMap = PartialSchemaRequirements{std::move(*resultReqs.finish())};
            return leftResult;
        }
        // Left and right don't all use the same key.

        if (PSRExpr::numLeaves(leftReqMap.getRoot()) != 1 ||
            PSRExpr::numLeaves(rightReqMap.getRoot()) != 1) {
            return {};
        }
        // Left and right don't all use the same key, but they both have exactly 1 entry.
        // In other words, leftKey != rightKey.

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
        tassert(7453902, "Unexpected binding in ComposeA", !leftReq.getBoundProjectionName());
        tassert(7453901, "Unexpected binding in ComposeA", !rightReq.getBoundProjectionName());

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
            return {{PartialSchemaRequirements{PSRExpr::makeSingularDNF(
                PartialSchemaKey{make<PathIdentity>()},
                PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                         std::move(intervalExpr),
                                         false /*isPerfOnly*/})}}};
        }

        return handleComposition<false /*isMultiplicative*/>(std::move(leftResult),
                                                             std::move(rightResult));
    }

    /**
     * Prepend a Get or Traverse to each Atom in the argument reqMap.
     *
     * 'n' specifies a single node to prepend. The child of 'n' is ignored. For example
     * if 'n' is 'Get [a] Get [b] Id' then this function prepends 'Get [a]' to 'inputResult'.
     *
     * Only considers atoms with an empty input binding. Atoms with a nonempty input binding are
     * ignored.
     */
    template <class T>
    static ResultType prependGetOrTraverse(const ABT& n, ResultType inputResult) {
        if (!inputResult) {
            return {};
        }
        if (inputResult->_bound) {
            return {};
        }

        PSRExpr::visitAnyShape(inputResult->_reqMap.getRoot(), [&](PartialSchemaEntry& entry) {
            ABT path = entry.first._path;

            // Updated key path to be now rooted at n, with existing key path as child.
            ABT appendedPath = n;
            std::swap(appendedPath.cast<T>()->getPath(), path);
            std::swap(path, appendedPath);

            entry.first._path = path;
        });
        return inputResult;
    }

    ResultType transport(const ABT& n, const PathGet& pathGet, ResultType inputResult) {
        return prependGetOrTraverse<PathGet>(n, std::move(inputResult));
    }

    ResultType transport(const ABT& n, const PathTraverse& pathTraverse, ResultType inputResult) {
        if (!inputResult) {
            return {};
        }

        if (!PSRExpr::isSingularDNF(inputResult->_reqMap.getRoot())) {
            // More than one requirement means we may have a conjunction inside a traverse.
            // We can change it to a traverse inside a conjunction, but that's an
            // over-approximation, so we have to keep the original predicate.
            inputResult->_retainPredicate = true;

            // Note that we could improve this by pushing traverse through disjunction without loss
            // of precision. 'Traverse (ComposeA X Y) == ComposeA (Traverse X) (Traverse Y)' because
            // Traverse is a disjunction over array elements, so it's ok to re-associate the
            // disjunctions.
        }

        auto result = prependGetOrTraverse<PathTraverse>(n, std::move(inputResult));
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
            PSRExpr::makeSingularDNF(PartialSchemaKey{make<PathIdentity>()},
                                     PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                                              std::move(*unionedInterval),
                                                              false /*isPerfOnly*/})}}};
    }

    ResultType transport(const ABT& n, const PathCompare& pathCompare, ResultType inputResult) {
        if (!inputResult) {
            return {};
        }
        if (!inputResult->_bound || !inputResult->_reqMap.isNoop()) {
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
            PSRExpr::makeSingularDNF(PartialSchemaKey{make<PathIdentity>()},
                                     PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                                              std::move(intervalExpr),
                                                              false /*isPerfOnly*/})}}};
    }

    ResultType transport(const ABT& n, const PathIdentity& pathIdentity) {
        return {{PartialSchemaRequirements{
            PSRExpr::makeSingularDNF(PartialSchemaKey{n},
                                     PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                                              IntervalReqExpr::makeSingularDNF(),
                                                              false /*isPerfOnly*/})}}};
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
                return {{PartialSchemaRequirements{PSRExpr::makeSingularDNF(
                    PartialSchemaKey{make<PathIdentity>()},
                    PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                             std::move(*conversion),
                                             false /*isPerfOnly*/})}}};
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
    if (reqMap.isNoop()) {
        return {};
    }

    // We need to determine either path or interval (or both).
    {
        bool trivialAtom = false;
        PSRExpr::visitAnyShape(reqMap.getRoot(), [&](PartialSchemaEntry& entry) {
            auto&& [key, req] = entry;
            if (key._path.is<PathIdentity>() && isIntervalReqFullyOpenDNF(req.getIntervals())) {
                trivialAtom = true;
            }
        });
        if (trivialAtom) {
            return {};
        }
    }

    // If we over-approximate, we need to switch all requirements to perf-only.
    if (result->_retainPredicate) {
        PSRExpr::visitDNF(reqMap.getRoot(), [&](PartialSchemaEntry& entry) {
            auto& [key, req] = entry;
            if (!req.getIsPerfOnly()) {
                req = {req.getBoundProjectionName(), req.getIntervals(), true /*isPerfOnly*/};
            }
        });
    }
    return result;
}

bool simplifyPartialSchemaReqPaths(const boost::optional<ProjectionName>& scanProjName,
                                   const MultikeynessTrie& multikeynessTrie,
                                   PartialSchemaRequirements& reqMap,
                                   ProjectionRenames& projectionRenames,
                                   const ConstFoldFn& constFold) {
    const auto simplifyFn = [&constFold](IntervalReqExpr::Node& intervals) -> bool {
        normalizeIntervals(intervals);
        auto simplified = simplifyDNFIntervals(intervals, constFold);
        if (simplified) {
            intervals = std::move(*simplified);
        }
        return simplified.has_value();
    };

    PSRExpr::Builder resultReqs;
    resultReqs.pushDisj();

    // TODO SERVER-73827 The builder should track trivially true/false correctly.
    // If any one conjunction is empty, the overall disjunction is trivially true.
    bool hasEmptyConjunction = false;

    PSRExpr::visitDisjuncts(reqMap.getRoot(), [&](const PSRExpr::Node& disjunct, size_t) {
        resultReqs.pushConj();
        boost::optional<std::pair<PartialSchemaKey, PartialSchemaRequirement>> prevEntry;

        // Track whether this conjunction has any arguments.
        bool hasAnyConjunct = false;

        const auto nextEntryFn = [&](PartialSchemaKey newKey, const PartialSchemaRequirement& req) {
            resultReqs.atom(std::move(prevEntry->first), std::move(prevEntry->second));
            prevEntry.reset({std::move(newKey), req});
            hasAnyConjunct = true;
        };

        // Simplify paths by eliminating unnecessary Traverse elements.

        PSRExpr::visitConjuncts(disjunct, [&](const PSRExpr::Node conjunct, size_t) {
            const auto& [key, req] = conjunct.cast<PSRExpr::Atom>()->getExpr();

            PartialSchemaKey newKey = key;

            bool simplified = false;
            const bool containedTraverse = checkPathContainsTraverse(newKey._path);
            if (key._projectionName == scanProjName && containedTraverse) {
                simplified = simplifyTraverseNonArray(newKey._path, multikeynessTrie);
            }
            // At this point we have simplified the path in newKey.

            if (!prevEntry) {
                prevEntry.reset({std::move(newKey), req});
                return;
            }
            if (prevEntry->first != newKey) {
                nextEntryFn(std::move(newKey), req);
                return;
            }

            auto& prevReq = prevEntry->second;
            auto resultIntervals = prevReq.getIntervals();
            combineIntervalsDNF(true /*intersect*/, resultIntervals, req.getIntervals());

            // Ensure that Traverse-less keys appear only once: we can move the conjunction into the
            // intervals and simplify. For traversing keys, check if interval is subsumed in the
            // other and if so, then combine.
            if (containedTraverse && !simplified &&
                !(resultIntervals == prevReq.getIntervals() ||
                  resultIntervals == req.getIntervals())) {
                // We cannot combine multikey paths where one interval does not subsume the other.
                nextEntryFn(std::move(newKey), req);
                return;
            }

            auto resultBoundProjName = prevReq.getBoundProjectionName();
            if (const auto& boundProjName = req.getBoundProjectionName()) {
                if (resultBoundProjName) {
                    // The existing name wins (stays in 'reqMap'). We tell the caller that the name
                    // "boundProjName" is available under "resultBoundProjName".
                    projectionRenames.emplace(*boundProjName, *resultBoundProjName);
                } else {
                    resultBoundProjName = boundProjName;
                }
            }

            if (constFold && !simplifyFn(resultIntervals)) {
                // TODO SERVER-73827 Consider having the BoolExpr builder handle simplifying away
                // trivial (always-true or always-false) clauses.

                // An always-false conjunct means the whole conjunction is always-false.
                // However, there can be other disjuncts, so we can't short-circuit the whole tree.
                // Create an explicit always-false atom.
                resultIntervals = IntervalReqExpr::makeSingularDNF(
                    BoundRequirement::makePlusInf(), BoundRequirement::makeMinusInf());
            }
            prevReq = {std::move(resultBoundProjName),
                       std::move(resultIntervals),
                       req.getIsPerfOnly() && prevReq.getIsPerfOnly()};
        });
        if (prevEntry) {
            resultReqs.atom(std::move(prevEntry->first), std::move(prevEntry->second));
            hasAnyConjunct = true;
        }

        resultReqs.pop();
        if (!hasAnyConjunct) {
            hasEmptyConjunction = true;
        }
    });

    boost::optional<PSRExpr::Node> builderResult = resultReqs.finish();
    if (!builderResult) {
        // TODO SERVER-73827 The builder should track trivially true/false correctly.
        if (hasEmptyConjunction) {
            // We have an empty conjunction -> trivially true.
            reqMap = PartialSchemaRequirements{};
            return false;
        } else {
            // We have an empty disjunction -> trivially false.
            return true;
        }
    }
    PartialSchemaRequirements newReqs{std::move(*builderResult)};

    if (constFold) {
        // Intersect and normalize intervals.
        const bool representable = newReqs.simplify([&](const PartialSchemaKey&,
                                                        PartialSchemaRequirement& req) -> bool {
            auto resultIntervals = req.getIntervals();
            if (!simplifyFn(resultIntervals)) {
                return false;
            }

            normalizeIntervals(resultIntervals);

            req = {req.getBoundProjectionName(), std::move(resultIntervals), req.getIsPerfOnly()};
            return true;
        });
        if (!representable) {
            // It simplifies to an always-false predicate, which we do not represent
            // as PartialSchemaRequirements.
            return true;
        }
    }

    reqMap = std::move(newReqs);
    return false;
}

/**
 * Try to compute the intersection of a an existing PartialSchemaRequirements object and a new
 * key/requirement pair.
 *
 * Returns false on "failure", which means the result was not representable. This happens if there
 * is a def-use dependency that combining into one PartialSchemaRequirements would break.
 *
 * The implementation works as follows:
 *     1. If the path of the new requirement already exists, we add it to the existing requirements.
 *     2. If the path does not exist, and does not refer to an existing entry, we add it.
 *     3. If we have an entry which binds the variable which the requirement uses.
 *        Append the paths (to rephrase the new requirement in terms of bindings
 *        visible in the input of the existing requirements) and retry. We will either
 *        add a new requirement or combine with an existing one.
 */
static bool intersectPartialSchemaReq(PartialSchemaRequirements& reqMap,
                                      PartialSchemaKey key,
                                      PartialSchemaRequirement req) {
    for (;;) {
        bool merged = false;
        const bool reqHasBoundProj = req.getBoundProjectionName().has_value();
        for (const auto& [existingKey, existingReq] : reqMap.conjuncts()) {
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
            reqMap.add(std::move(key), std::move(req));
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
    PSRExpr::visitAnyShape(rhs.getRoot(), [](const PartialSchemaEntry& e) {
        tassert(7155010,
                "isSubsetOfPartialSchemaReq expects 'rhs' to have no projections",
                !e.second.getBoundProjectionName());
    });

    PartialSchemaRequirements intersection = lhs;
    const bool success = intersectPartialSchemaReq(intersection, rhs);
    tassert(6624172, "Intersection should succeed since 'rhs' has no projections", success);

    ProjectionRenames renames_unused;
    const bool hasEmptyInterval = simplifyPartialSchemaReqPaths(boost::none /*scanProjName*/,
                                                                {} /*multikeynessTrie*/,
                                                                intersection,
                                                                renames_unused,
                                                                {} /*constFold*/);
    tassert(6624169,
            "Cannot detect empty intervals without providing a constant folder",
            !hasEmptyInterval);

    return intersection == lhs;
}

bool intersectPartialSchemaReq(PartialSchemaRequirements& target,
                               const PartialSchemaRequirements& source) {
    if (!PSRExpr::isSingletonDisjunction(target.getRoot()) ||
        !PSRExpr::isSingletonDisjunction(source.getRoot())) {
        return false;
    }

    for (const auto& [key, req] : source.conjuncts()) {
        if (!intersectPartialSchemaReq(target, key, req)) {
            return false;
        }
    }

    return true;
}

PartialSchemaRequirements unionPartialSchemaReq(PartialSchemaRequirements&& left,
                                                PartialSchemaRequirements&& right) {
    tassert(
        7453911, "unionPartialSchemaReq assumes DNF", left.getRoot().is<PSRExpr::Disjunction>());
    tassert(
        7453910, "unionPartialSchemaReq assumes DNF", right.getRoot().is<PSRExpr::Disjunction>());

    PSRExpr::Disjunction& leftDisj = *left.getRoot().cast<PSRExpr::Disjunction>();
    PSRExpr::Disjunction& rightDisj = *right.getRoot().cast<PSRExpr::Disjunction>();
    auto resultNodes = std::move(leftDisj.nodes());
    resultNodes.insert(resultNodes.end(),
                       std::make_move_iterator(rightDisj.nodes().begin()),
                       std::make_move_iterator(rightDisj.nodes().end()));
    return PartialSchemaRequirements{PSRExpr::make<PSRExpr::Disjunction>(std::move(resultNodes))};
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
 * Pad compound interval with supplied simple interval.
 */
static void extendCompoundInterval(const IndexCollationSpec& indexCollationSpec,
                                   CompoundIntervalReqExpr::Node& expr,
                                   const size_t indexField,
                                   IntervalReqExpr::Node interval) {
    const bool reverse = indexCollationSpec.at(indexField)._op == CollationOp::Descending;
    if (!combineCompoundIntervalsDNF(expr, std::move(interval), reverse)) {
        uasserted(6624159, "Cannot combine compound interval with simple interval.");
    }
}

/**
 * Pad compound interval with unconstrained simple interval.
 */
static void padCompoundInterval(const IndexCollationSpec& indexCollationSpec,
                                CompoundIntervalReqExpr::Node& expr,
                                const size_t indexField) {
    const bool reverse = indexCollationSpec.at(indexField)._op == CollationOp::Descending;
    padCompoundIntervalsDNF(expr, reverse);
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

        for (size_t i = 0; i < indexCollationSpec.size() - indexField; i++) {
            // Pad old prefix with open intervals to the end.
            padCompoundInterval(indexCollationSpec, eqPrefixes.back()._interval, i + indexField);
        }

        if (eqPrefixes.size() >= maxIndexEqPrefixes) {
            // Too many equality prefixes.
            return false;
        }
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
            extendCompoundInterval(indexCollationSpec,
                                   eqPrefixes.back()._interval,
                                   i,
                                   IntervalReqExpr::makeSingularDNF(eqBound, eqBound));
        }
    }

    return true;
}

static bool computeCandidateIndexEntry(PrefixId& prefixId,
                                       const PartialSchemaRequirements& reqMap,
                                       const size_t maxIndexEqPrefixes,
                                       PartialSchemaKeySet unsatisfiedKeys,
                                       const ProjectionName& scanProjName,
                                       const QueryHints& hints,
                                       const ConstFoldFn& constFold,
                                       const IndexCollationSpec& indexCollationSpec,
                                       CandidateIndexEntry& entry) {
    auto& fieldProjMap = entry._fieldProjectionMap;
    auto& eqPrefixes = entry._eqPrefixes;
    auto& correlatedProjNames = entry._correlatedProjNames;
    auto& predTypes = entry._predTypes;

    // Add open interval for the first equality prefix.
    eqPrefixes.emplace_back(0);

    // For each component of the index,
    for (size_t indexField = 0; indexField < indexCollationSpec.size(); indexField++) {
        const auto& indexCollationEntry = indexCollationSpec.at(indexField);
        const bool reverse = indexCollationEntry._op == CollationOp::Descending;
        bool foundSuitableField = false;

        PartialSchemaKey indexKey{scanProjName, indexCollationEntry._path};
        if (auto result = reqMap.findFirstConjunct(indexKey)) {
            if (const auto& [queryPredPos, req] = *result; hints._fastIndexNullHandling ||
                !req.getIsPerfOnly() || !req.mayReturnNull(constFold)) {
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
                    while (predTypes.size() < indexCollationSpec.size()) {
                        // Pad remaining index key as "unbound".
                        predTypes.push_back(IndexFieldPredType::Unbound);
                    }
                    break;
                }
                if (eqPrefixes.size() > 1 && entry._intervalPrefixSize == 0) {
                    // Need to constrain at least one field per prefix.
                    return false;
                }

                foundSuitableField = true;
                unsatisfiedKeys.erase(indexKey);
                entry._intervalPrefixSize++;

                eqPrefixes.back()._predPosSet.insert(queryPredPos);

                if (const auto& boundProjName = req.getBoundProjectionName()) {
                    // Include bounds projection into index spec.
                    const bool inserted =
                        fieldProjMap._fieldProjections
                            .emplace(encodeIndexKeyName(indexField), *boundProjName)
                            .second;
                    invariant(inserted);
                }

                if (auto singularInterval = IntervalReqExpr::getSingularDNF(requiredInterval)) {
                    if (singularInterval->isEquality()) {
                        predTypes.push_back(IndexFieldPredType::SimpleEquality);
                    } else {
                        predTypes.push_back(IndexFieldPredType::SimpleInequality);
                    }
                } else {
                    predTypes.push_back(IndexFieldPredType::Compound);
                }
            }
        }

        if (!foundSuitableField) {
            // We cannot constrain the current index field.
            padCompoundInterval(indexCollationSpec, eqPrefixes.back()._interval, indexField);
            predTypes.push_back(IndexFieldPredType::Unbound);
        }
    }

    // Compute residual predicates from unsatisfied partial schema keys.
    ResidualRequirements::Builder residualReqs;
    residualReqs.pushDisj().pushConj();
    for (auto queryKeyIt = unsatisfiedKeys.begin(); queryKeyIt != unsatisfiedKeys.end();) {
        const auto& queryKey = *queryKeyIt;
        bool satisfied = false;

        for (size_t indexField = 0; indexField < indexCollationSpec.size(); indexField++) {
            if (const auto fusedPath =
                    fuseIndexPath(queryKey._path, indexCollationSpec.at(indexField)._path);
                fusedPath._suffix) {
                auto result = reqMap.findFirstConjunct(queryKey);
                tassert(6624158, "QueryKey must exist in the requirements map", result);
                const auto& [index, req] = *result;

                if (hints._forceIndexScanForPredicates &&
                    !isIntervalReqFullyOpenDNF(req.getIntervals())) {
                    // We need to cover all predicates with index scans.
                    break;
                }

                if (!req.getIsPerfOnly()) {
                    // Only regular requirements are added to residual predicates.
                    const ProjectionName& tempProjName = getExistingOrTempProjForFieldName(
                        prefixId, FieldNameType{encodeIndexKeyName(indexField)}, fieldProjMap);
                    residualReqs.atom(
                        PartialSchemaKey{tempProjName, std::move(*fusedPath._suffix)}, req, index);
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

    entry._residualRequirements = residualReqs.finish();

    if (entry._intervalPrefixSize == 0 && !entry._residualRequirements) {
        // Need to encode at least one query requirement in the index bounds.
        return false;
    }

    return true;
}

static bool checkCanFuse(const MultikeynessTrie& tree1, const MultikeynessTrie& tree2) {
    if (tree1.isMultiKey && tree2.isMultiKey) {
        return false;
    }

    for (const auto& [key, child] : tree2.children) {
        if (const auto it = tree1.children.find(key); it != tree1.children.cend()) {
            if (!checkCanFuse(it->second, child)) {
                return false;
            }
        }
    }

    return true;
}

CandidateIndexes computeCandidateIndexes(PrefixId& prefixId,
                                         const ProjectionName& scanProjectionName,
                                         const PartialSchemaRequirements& reqMap,
                                         const ScanDefinition& scanDef,
                                         const QueryHints& hints,
                                         const ConstFoldFn& constFold) {
    // A candidate index is one that can directly satisfy the SargableNode, without using
    // any other indexes. Typically a disjunction would require unioning two different indexes,
    // so we bail out if there's a nontrivial disjunction here.
    if (!PSRExpr::isSingletonDisjunction(reqMap.getRoot())) {
        return {};
    }

    // Contains one instance for each unmatched key.
    PartialSchemaKeySet unsatisfiedKeysInitial;

    // Tree of containing the current set of index paths. Used to check if we can encode a query
    // predicate into index bounds or as residual. For example, if we have a query with two multikey
    // paths which share a multikey component: Get "a" Traverse Get "b" Traverse Id and Get "a"
    // Traverse Get "c" Traverse Id we would not admit both to be satisfied via an index scan. By
    // contrast, if they shared a non-multikey component (Get "a" only) or they did not share any
    // component (e.g. Get "a" Traverse Get "b" Id and Get "c" Traverse Get "d" Id) then the two
    // paths may be satisfied via the same index scan.
    MultikeynessTrie indexPathTrie;

    for (const auto& [key, req] : reqMap.conjuncts()) {
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

        const auto currentTrie = MultikeynessTrie::fromIndexPath(key._path);
        if (!checkCanFuse(indexPathTrie, currentTrie)) {
            return {};
        }
        indexPathTrie.merge(currentTrie);
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
                                                            hints,
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
    auto& fieldProjMap = result._fieldProjectionMap;
    ResidualRequirements::Builder residReqs;

    bool invalid = false;
    size_t entryIndex = 0;
    residReqs.pushDisj();
    PSRExpr::visitDisjuncts(reqMap.getRoot(), [&](const PSRExpr::Node& disjunct, size_t) {
        residReqs.pushConj();
        PSRExpr::visitConjuncts(disjunct, [&](const PSRExpr::Node& conjunct, size_t) {
            PSRExpr::visitAtom(conjunct, [&](const PartialSchemaEntry& e) {
                if (invalid) {
                    // Short circuit if we're going to return {} anyway.
                    return;
                }
                const auto& [key, req] = e;
                if (req.getIsPerfOnly()) {
                    // Ignore perf only requirements.
                    return;
                }
                if (key._projectionName != rootProj) {
                    // We are not sitting right above a ScanNode.
                    invalid = true;
                    return;
                }

                if (auto pathGet = key._path.cast<PathGet>(); pathGet != nullptr) {
                    const FieldNameType& fieldName = pathGet->name();

                    // Extract a new requirements path with removed simple paths.
                    // For example if we have a key Get "a" Traverse Compare = 0 we leave
                    // only Traverse Compare 0.
                    if (const auto& boundProjName = req.getBoundProjectionName();
                        boundProjName && pathGet->getPath().is<PathIdentity>()) {
                        const auto [it, insertedInFPM] =
                            fieldProjMap._fieldProjections.emplace(fieldName, *boundProjName);

                        if (!insertedInFPM) {
                            residReqs.atom(PartialSchemaKey{it->second, make<PathIdentity>()},
                                           PartialSchemaRequirement{req.getBoundProjectionName(),
                                                                    req.getIntervals(),
                                                                    false /*isPerfOnly*/},
                                           entryIndex);
                        } else if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                            residReqs.atom(
                                PartialSchemaKey{*boundProjName, make<PathIdentity>()},
                                PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                                         req.getIntervals(),
                                                         false /*isPerfOnly*/},
                                entryIndex);
                        }
                    } else {
                        const ProjectionName& tempProjName =
                            getExistingOrTempProjForFieldName(prefixId, fieldName, fieldProjMap);
                        residReqs.atom(
                            PartialSchemaKey{tempProjName, pathGet->getPath()}, req, entryIndex);
                    }
                } else {
                    // Move other conditions into the residual map.
                    fieldProjMap._rootProjection = rootProj;
                    residReqs.atom(key, req, entryIndex);
                }

                entryIndex++;
            });
        });
        residReqs.pop();
    });
    if (invalid) {
        return {};
    }

    result._residualRequirements = residReqs.finish();
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

/**
 * Identifies all Eq or EqMember nodes in the childResults. If there is more than one,
 * they are consolidated into one larger EqMember node with a new array containing
 * all of the individual node's value.
 */
static void consolidateEqDisjunctions(ABTVector& childResults) {
    ABTVector newResults;
    const auto [eqMembersTag, eqMembersVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard guard{eqMembersTag, eqMembersVal};
    const auto eqMembersArray = sbe::value::getArrayView(eqMembersVal);

    for (size_t index = 0; index < childResults.size(); index++) {
        ABT& child = childResults.at(index);
        if (const auto pathCompare = child.cast<PathCompare>();
            pathCompare != nullptr && pathCompare->getVal().is<Constant>()) {
            switch (pathCompare->op()) {
                case Operations::EqMember: {
                    // Unpack this node's values into the new eqMembersArray.
                    const auto [arrayTag, arrayVal] = pathCompare->getVal().cast<Constant>()->get();
                    if (arrayTag != sbe::value::TypeTags::Array) {
                        continue;
                    }

                    const auto valsArray = sbe::value::getArrayView(arrayVal);
                    for (size_t j = 0; j < valsArray->size(); j++) {
                        const auto [tag, val] = valsArray->getAt(j);
                        // If this is found to be a bottleneck, could be implemented with moving,
                        // rather than copying, the values into the array (same in Eq case).
                        const auto [newTag, newVal] = sbe::value::copyValue(tag, val);
                        eqMembersArray->push_back(newTag, newVal);
                    }
                    break;
                }
                case Operations::Eq: {
                    // Copy this node's value into the new eqMembersArray.
                    const auto [tag, val] = pathCompare->getVal().cast<Constant>()->get();
                    const auto [newTag, newVal] = sbe::value::copyValue(tag, val);
                    eqMembersArray->push_back(newTag, newVal);
                    break;
                }
                default:
                    newResults.emplace_back(std::move(child));
                    continue;
            }
        } else {
            newResults.emplace_back(std::move(child));
        }
    }

    // Add a node to the newResults with the combined Eq/EqMember values under one EqMember; if
    // there is only one value, add an Eq node with that value.
    if (eqMembersArray->size() > 1) {
        guard.reset();
        newResults.emplace_back(
            make<PathCompare>(Operations::EqMember, make<Constant>(eqMembersTag, eqMembersVal)));
    } else if (eqMembersArray->size() == 1) {
        const auto [eqConstantTag, eqConstantVal] =
            sbe::value::copyValue(eqMembersArray->getAt(0).first, eqMembersArray->getAt(0).second);
        newResults.emplace_back(
            make<PathCompare>(Operations::Eq, make<Constant>(eqConstantTag, eqConstantVal)));
    }

    std::swap(childResults, newResults);
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
        if (childResults.size() > 1) {
            // Consolidate Eq and EqMember disjunctions, then construct a balanced additive
            // composition tree.
            consolidateEqDisjunctions(childResults);
            maybeComposePaths<PathComposeA>(childResults);
        }
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

/**
 * Build the EvalFilter and EvalPath expressions corresponding to the given requirement and path.
 *
 * Semantically the EvalPath comes first: the EvalFilter may assume the EvalPath is bound to
 * req.getBoundProjectionName().
 */
static std::pair<boost::optional<ABT>, boost::optional<ABT>> makeLoweredExpressionsForReq(
    const PartialSchemaKey& key,
    const PartialSchemaRequirement& req,
    const PathToIntervalFn& pathToInterval) {
    boost::optional<ABT> evalPath;
    boost::optional<ABT> evalFilter;

    PartialSchemaReqLowerTransport transport(req.getBoundProjectionName().has_value(),
                                             pathToInterval);
    ABT path = transport.lower(req.getIntervals());
    const bool hasIntervals = path.is<PathIdentity>();

    ABT inputVar = make<Variable>(*key._projectionName);
    if (const auto& boundProjName = req.getBoundProjectionName()) {
        evalPath = make<EvalPath>(key._path, std::move(inputVar));

        if (!hasIntervals) {
            evalFilter = make<EvalFilter>(std::move(path), make<Variable>(*boundProjName));
        }
    } else {
        uassert(6624162,
                "If we do not have a bound projection, then we should have a proper path",
                !hasIntervals);

        path = PathAppender::append(key._path, std::move(path));
        evalFilter = make<EvalFilter>(std::move(path), std::move(inputVar));
    }

    return {std::move(evalPath), std::move(evalFilter)};
}

void lowerPartialSchemaRequirement(const PartialSchemaKey& key,
                                   const PartialSchemaRequirement& req,
                                   const PathToIntervalFn& pathToInterval,
                                   const boost::optional<CEType> residualCE,
                                   PhysPlanBuilder& builder) {
    auto [evalPath, evalFilter] = makeLoweredExpressionsForReq(key, req, pathToInterval);

    if (const auto& boundProjName = req.getBoundProjectionName()) {
        builder.make<EvaluationNode>(
            residualCE, *boundProjName, std::move(*evalPath), std::move(builder._node));
    }

    if (evalFilter) {
        builder.make<FilterNode>(residualCE, std::move(*evalFilter), std::move(builder._node));
    }
}

void lowerPartialSchemaRequirements(boost::optional<CEType> scanGroupCE,
                                    std::vector<SelectivityType> indexPredSels,
                                    ResidualRequirementsWithOptionalCE::Node requirements,
                                    const PathToIntervalFn& pathToInterval,
                                    PhysPlanBuilder& builder) {
    sortResidualRequirements(requirements);

    // If there is a single Conjunction, build a sequence of FilterNode (one for each conjunct).
    if (ResidualRequirementsWithOptionalCE::isSingletonDisjunction(requirements)) {
        ResidualRequirementsWithOptionalCE::visitDNF(
            requirements, [&](const ResidualRequirementWithOptionalCE& entry) {
                auto residualCE = scanGroupCE;
                if (residualCE) {
                    if (!indexPredSels.empty()) {
                        *residualCE *= ce::conjExponentialBackoff(indexPredSels);
                    }
                    if (entry._ce && *scanGroupCE > 0.0) {
                        // Compute the selectivity after we assign CE, which is the "input" to the
                        // cost.
                        indexPredSels.push_back(*entry._ce / *scanGroupCE);
                    }
                }

                lowerPartialSchemaRequirement(
                    entry._key, entry._req, pathToInterval, residualCE, builder);
            });
        return;
    }

    // For multiple Conjunctions, build a top-level Or expression representing the composition.
    ABTVector toOr;
    std::vector<SelectivityType> disjSels;
    ResidualRequirementsWithOptionalCE::visitDisjuncts(
        requirements,
        [&](const typename ResidualRequirementsWithOptionalCE::Node& child, const size_t) {
            ABTVector toAnd;
            std::vector<SelectivityType> conjSels;

            ResidualRequirementsWithOptionalCE::visitConjuncts(
                child,
                [&](const typename ResidualRequirementsWithOptionalCE::Node& atom, const size_t) {
                    ResidualRequirementsWithOptionalCE::visitAtom(
                        atom, [&](const ResidualRequirementWithOptionalCE& entry) {
                            const auto& [key, req, ce] = entry;
                            auto [evalPath, evalFilter] =
                                makeLoweredExpressionsForReq(key, req, pathToInterval);
                            tassert(7506401, "Requirement must have an interval", evalFilter);
                            tassert(7506402, "Requirement should not bind", !evalPath);

                            toAnd.push_back(std::move(*evalFilter));
                            if (ce && scanGroupCE && *scanGroupCE > 0.0) {
                                conjSels.push_back(*ce / *scanGroupCE);
                            }
                        });
                });

            if (!conjSels.empty()) {
                disjSels.push_back(ce::conjExponentialBackoff(conjSels));
            }
            toOr.push_back(makeBalancedBooleanOpTree(Operations::And, std::move(toAnd)));
        });

    boost::optional<CEType> finalFilterCE = scanGroupCE;
    if (!disjSels.empty()) {
        indexPredSels.push_back(ce::disjExponentialBackoff(disjSels));
        finalFilterCE = *scanGroupCE * ce::conjExponentialBackoff(indexPredSels);
    }
    builder.make<FilterNode>(finalFilterCE,
                             makeBalancedBooleanOpTree(Operations::Or, std::move(toOr)),
                             std::move(builder._node));
}

void sortResidualRequirements(ResidualRequirementsWithOptionalCE::Node& residualReqs) {
    ResidualRequirementsWithOptionalCE::visitDisjuncts(
        residualReqs, [](ResidualRequirementsWithOptionalCE::Node& child, const size_t) {
            // Collect the estimated costs of each child under a conjunction. Assume it is
            // more expensive to deliver a bound projection than to just filter.
            std::vector<std::pair<double, size_t>> costs;
            size_t numConjuncts = 0;

            ResidualRequirementsWithOptionalCE::visitConjuncts(
                child, [&](ResidualRequirementsWithOptionalCE::Node& atom, const size_t index) {
                    ResidualRequirementsWithOptionalCE::visitAtom(
                        atom, [&](ResidualRequirementWithOptionalCE& entry) {
                            numConjuncts++;

                            if (entry._ce) {
                                size_t multiplier = 0;
                                if (entry._req.getBoundProjectionName()) {
                                    multiplier++;
                                }
                                if (!isIntervalReqFullyOpenDNF(entry._req.getIntervals())) {
                                    multiplier++;
                                }
                                costs.emplace_back(entry._ce->_value * multiplier, index);
                            }
                        });
                });

            // The entries may not have CE values, in which case there is no way to sort them.
            if (costs.empty()) {
                return;
            }
            tassert(7506403,
                    "Residual requirements missing cardinality estimate for at least one atom",
                    costs.size() == numConjuncts);

            std::sort(costs.begin(), costs.end());
            auto& atoms = child.cast<ResidualRequirementsWithOptionalCE::Conjunction>()->nodes();
            for (size_t index = 0; index < atoms.size(); index++) {
                const size_t targetIndex = costs.at(index).second;
                if (index < targetIndex) {
                    std::swap(atoms.at(index), atoms.at(targetIndex));
                }
            }
        });
}

ResidualRequirementsWithOptionalCE::Node createResidualReqsWithCE(
    const ResidualRequirements::Node& residReqs, const PartialSchemaKeyCE& partialSchemaKeyCE) {
    ResidualRequirementsWithOptionalCE::Builder b;
    b.pushDisj();

    ResidualRequirements::visitDisjuncts(
        residReqs, [&](const ResidualRequirements::Node& child, const size_t) {
            b.pushConj();

            ResidualRequirements::visitConjuncts(
                child, [&](const ResidualRequirements::Node& atom, const size_t) {
                    ResidualRequirements::visitAtom(atom, [&](const ResidualRequirement& req) {
                        b.atom(req._key, req._req, partialSchemaKeyCE.at(req._entryIndex).second);
                    });
                });

            b.pop();
        });

    return std::move(*b.finish());
}

ResidualRequirementsWithOptionalCE::Node createResidualReqsWithEmptyCE(const PSRExpr::Node& reqs) {
    ResidualRequirementsWithOptionalCE::Builder b;
    b.pushDisj();

    PSRExpr::visitDisjuncts(reqs, [&](const PSRExpr::Node& child, const size_t) {
        b.pushConj();

        PSRExpr::visitConjuncts(child, [&](const PSRExpr::Node& atom, const size_t) {
            PSRExpr::visitAtom(atom, [&](const PartialSchemaEntry& entry) {
                b.atom(entry.first, entry.second, boost::none);
            });
        });

        b.pop();
    });

    return std::move(*b.finish());
}

void applyProjectionRenames(ProjectionRenames projectionRenames, ABT& node) {
    for (auto&& [targetProjName, sourceProjName] : projectionRenames) {
        node = make<EvaluationNode>(
            std::move(targetProjName), make<Variable>(std::move(sourceProjName)), std::move(node));
    }
}

void removeRedundantResidualPredicates(const ProjectionNameOrderPreservingSet& requiredProjections,
                                       boost::optional<ResidualRequirements::Node>& residualReqs,
                                       FieldProjectionMap& fieldProjectionMap) {
    ProjectionNameSet residualTempProjections;

    // Remove unused residual requirements.
    if (residualReqs) {
        ResidualRequirements::Builder newReqs;
        newReqs.pushDisj();

        ResidualRequirements::visitDisjuncts(
            *residualReqs, [&](const ResidualRequirements::Node& child, const size_t) {
                newReqs.pushConj();

                ResidualRequirements::visitConjuncts(
                    child, [&](const ResidualRequirements::Node& atom, const size_t) {
                        ResidualRequirements::visitAtom(
                            atom, [&](const ResidualRequirement& residReq) {
                                auto& [key, req, ce] = residReq;

                                if (const auto& boundProjName = req.getBoundProjectionName();
                                    boundProjName && !requiredProjections.find(*boundProjName)) {
                                    if (isIntervalReqFullyOpenDNF(req.getIntervals())) {
                                        return;
                                    }

                                    residualTempProjections.insert(*key._projectionName);

                                    // We do not use the output binding, but we still want to
                                    // filter.
                                    tassert(
                                        6624163,
                                        "Should not be seeing a perf-only predicate as residual",
                                        !req.getIsPerfOnly());
                                    newReqs.atom(std::move(key),
                                                 PartialSchemaRequirement{
                                                     boost::none /*boundProjectionName*/,
                                                     std::move(req.getIntervals()),
                                                     req.getIsPerfOnly()},
                                                 ce);
                                } else {
                                    residualTempProjections.insert(*key._projectionName);
                                    newReqs.atom(std::move(key), std::move(req), ce);
                                }
                            });
                    });

                newReqs.pop();
            });
        auto result = newReqs.finish();
        std::swap(result, residualReqs);
    }

    // Remove unused projections from the field projection map.
    auto& fieldProjMap = fieldProjectionMap._fieldProjections;
    for (auto it = fieldProjMap.begin(); it != fieldProjMap.end();) {
        if (const ProjectionName& projName = it->second;
            !requiredProjections.find(projName) && residualTempProjections.count(projName) == 0) {
            fieldProjMap.erase(it++);
        } else {
            it++;
        }
    }
}

PhysPlanBuilder lowerRIDIntersectGroupBy(PrefixId& prefixId,
                                         const ProjectionName& ridProjName,
                                         const CEType intersectedCE,
                                         const CEType leftCE,
                                         const CEType rightCE,
                                         const properties::PhysProps& physProps,
                                         const properties::PhysProps& leftPhysProps,
                                         const properties::PhysProps& rightPhysProps,
                                         PhysPlanBuilder leftChild,
                                         PhysPlanBuilder rightChild,
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

    leftChild.make<EvaluationNode>(
        leftCE, sideIdProjectionName, Constant::int64(0), std::move(leftChild._node));
    childProps.emplace_back(&leftChild._node.cast<EvaluationNode>()->getChild(), leftPhysProps);

    rightChild.make<EvaluationNode>(
        rightCE, sideIdProjectionName, Constant::int64(1), std::move(rightChild._node));
    childProps.emplace_back(&rightChild._node.cast<EvaluationNode>()->getChild(), rightPhysProps);

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
            leftChild.make<EvaluationNode>(leftCE,
                                           tempProjectionName,
                                           make<Variable>(projectionName),
                                           std::move(leftChild._node));
            rightChild.make<EvaluationNode>(
                rightCE, tempProjectionName, Constant::nothing(), std::move(rightChild._node));
        } else {
            leftChild.make<EvaluationNode>(
                leftCE, tempProjectionName, Constant::nothing(), std::move(leftChild._node));
            rightChild.make<EvaluationNode>(rightCE,
                                            tempProjectionName,
                                            make<Variable>(projectionName),
                                            std::move(rightChild._node));
        }

        aggExpressions.emplace_back(
            make<FunctionCall>("$max", makeSeq(make<Variable>(tempProjectionName))));
        aggProjectionNames.push_back(projectionName);
    }

    PhysPlanBuilder result;
    result.make<UnionNode>(leftCE + rightCE,
                           std::move(unionProjections),
                           makeSeq(std::move(leftChild._node), std::move(rightChild._node)));
    result.merge(leftChild);
    result.merge(rightChild);

    result.make<GroupByNode>(intersectedCE,
                             ProjectionNameVector{ridProjName},
                             std::move(aggProjectionNames),
                             std::move(aggExpressions),
                             std::move(result._node));

    result.make<FilterNode>(
        intersectedCE,
        make<EvalFilter>(
            make<PathCompare>(Operations::Eq, Constant::int64(2)),
            make<FunctionCall>("getArraySize", makeSeq(make<Variable>(sideSetProjectionName)))),
        std::move(result._node));

    return result;
}

PhysPlanBuilder lowerRIDIntersectHashJoin(PrefixId& prefixId,
                                          const ProjectionName& ridProjName,
                                          const CEType intersectedCE,
                                          const CEType leftCE,
                                          const CEType rightCE,
                                          const properties::PhysProps& leftPhysProps,
                                          const properties::PhysProps& rightPhysProps,
                                          PhysPlanBuilder leftChild,
                                          PhysPlanBuilder rightChild,
                                          ChildPropsType& childProps) {
    using namespace properties;

    ProjectionName rightRIDProjName = prefixId.getNextId("rid");
    rightChild.make<EvaluationNode>(
        rightCE, rightRIDProjName, make<Variable>(ridProjName), std::move(rightChild._node));
    ABT* rightChildPtr = &rightChild._node.cast<EvaluationNode>()->getChild();

    auto rightProjections =
        getPropertyConst<ProjectionRequirement>(rightPhysProps).getProjections();
    rightProjections.erase(ridProjName);
    rightProjections.emplace_back(rightRIDProjName);
    ProjectionNameVector sortedProjections = rightProjections.getVector();
    std::sort(sortedProjections.begin(), sortedProjections.end());

    // Use a union node to restrict the rid projection name coming from the right child in order
    // to ensure we do not have the same rid from both children. This node is optimized away
    // during lowering.
    restrictProjections(std::move(sortedProjections), rightCE, rightChild);

    PhysPlanBuilder result;
    result.make<HashJoinNode>(intersectedCE,
                              JoinType::Inner,
                              ProjectionNameVector{ridProjName},
                              ProjectionNameVector{std::move(rightRIDProjName)},
                              std::move(leftChild._node),
                              std::move(rightChild._node));
    result.merge(leftChild);
    result.merge(rightChild);

    childProps.emplace_back(&result._node.cast<HashJoinNode>()->getLeftChild(), leftPhysProps);
    childProps.emplace_back(rightChildPtr, rightPhysProps);

    return result;
}

PhysPlanBuilder lowerRIDIntersectMergeJoin(PrefixId& prefixId,
                                           const ProjectionName& ridProjName,
                                           const CEType intersectedCE,
                                           const CEType leftCE,
                                           const CEType rightCE,
                                           const properties::PhysProps& leftPhysProps,
                                           const properties::PhysProps& rightPhysProps,
                                           PhysPlanBuilder leftChild,
                                           PhysPlanBuilder rightChild,
                                           ChildPropsType& childProps) {
    using namespace properties;

    ProjectionName rightRIDProjName = prefixId.getNextId("rid");
    rightChild.make<EvaluationNode>(
        rightCE, rightRIDProjName, make<Variable>(ridProjName), std::move(rightChild._node));
    ABT* rightChildPtr = &rightChild._node.cast<EvaluationNode>()->getChild();

    auto rightProjections =
        getPropertyConst<ProjectionRequirement>(rightPhysProps).getProjections();
    rightProjections.erase(ridProjName);
    rightProjections.emplace_back(rightRIDProjName);
    ProjectionNameVector sortedProjections = rightProjections.getVector();
    std::sort(sortedProjections.begin(), sortedProjections.end());

    // Use a union node to restrict the rid projection name coming from the right child in order
    // to ensure we do not have the same rid from both children. This node is optimized away
    // during lowering.
    restrictProjections(std::move(sortedProjections), rightCE, rightChild);

    PhysPlanBuilder result;
    result.make<MergeJoinNode>(intersectedCE,
                               ProjectionNameVector{ridProjName},
                               ProjectionNameVector{std::move(rightRIDProjName)},
                               std::vector<CollationOp>{CollationOp::Ascending},
                               std::move(leftChild._node),
                               std::move(rightChild._node));
    result.merge(leftChild);
    result.merge(rightChild);

    childProps.emplace_back(&result._node.cast<MergeJoinNode>()->getLeftChild(), leftPhysProps);
    childProps.emplace_back(rightChildPtr, rightPhysProps);

    return result;
}

/**
 * Generates a distinct scan plan. This is a type of index scan which iterates over the distinct
 * (unique) values in a particular index range. The basic outline of the plan is the following:
 *
 *     SpoolProducer (<spoolId>, <outerProjNames>)
 *     Union (<outerProjNames>)
 *         Limit 1
 *         IxScan(<outerFPM>, <outerInterval>, reverse)
 *
 *         NLJ (correlated = <innerProjNames>)
 *             SpoolConsumer (<spoolId>, <innerProjNames>)
 *
 *             Limit 1
 *             IxScan(<outerProjNames>, <innerInterval>, reverse)
 *
 *  For a particular index, we generate a spooling plan (see corresponding spool producer and
 * consumer nodes), which first seeds the iteration with the base case represented by the outer
 * index scan and associated parameters. Then we iterate in the recursive case where we re-open the
 * inner index scan on every new combination of values we receive from the spool.
 */
static PhysPlanBuilder generateDistinctScan(const std::string& scanDefName,
                                            const std::string& indexDefName,
                                            const bool reverse,
                                            const CEType ce,
                                            const int spoolId,
                                            ProjectionNameVector outerProjNames,
                                            FieldProjectionMap outerFPM,
                                            CompoundIntervalRequirement outerInterval,
                                            ProjectionNameVector innerProjNames,
                                            FieldProjectionMap innerFPM,
                                            CompoundIntervalRequirement innerInterval) {
    ProjectionNameSet innerProjNameSet;
    for (const auto& projName : innerProjNames) {
        innerProjNameSet.insert(projName);
    }

    PhysPlanBuilder result;
    result.make<IndexScanNode>(
        ce, std::move(innerFPM), scanDefName, indexDefName, std::move(innerInterval), reverse);

    // Advance to the next unique key.
    result.make<LimitSkipNode>(ce, properties::LimitSkipRequirement{1, 0}, std::move(result._node));

    PhysPlanBuilder spoolCons;
    spoolCons.make<SpoolConsumerNode>(
        ce, SpoolConsumerType::Stack, spoolId, std::move(innerProjNames));

    // Inner correlated join.
    result.make<NestedLoopJoinNode>(ce,
                                    JoinType::Inner,
                                    std::move(innerProjNameSet),
                                    Constant::boolean(true),
                                    std::move(spoolCons._node),
                                    std::move(result._node));
    result.merge(spoolCons);

    // Outer index scan node.
    PhysPlanBuilder outerIndexScan;
    outerIndexScan.make<IndexScanNode>(
        ce, std::move(outerFPM), scanDefName, indexDefName, std::move(outerInterval), reverse);

    // Limit to one result to seed the distinct scan.
    outerIndexScan.make<LimitSkipNode>(
        ce, properties::LimitSkipRequirement{1, 0}, std::move(outerIndexScan._node));

    result.make<UnionNode>(
        ce, outerProjNames, makeSeq(std::move(outerIndexScan._node), std::move(result._node)));
    result.merge(outerIndexScan);

    result.make<SpoolProducerNode>(ce,
                                   SpoolProducerType::Lazy,
                                   spoolId,
                                   std::move(outerProjNames),
                                   Constant::boolean(true),
                                   std::move(result._node));

    return result;
}

/**
 * This transport is responsible for encoding a set of equality prefixes into a physical plan. Each
 * equality prefix represents a subset of predicates to be evaluated using a particular index
 * (referenced by "indexDefName"). The combined set of predicates is structured into equality
 * prefixes such that each prefix begins with a (possibly empty) prefix of equality (point)
 * predicates on the respective index fields from the start, followed by at most one inequality
 * predicate, followed by a (possibly empty) suffix of unbound fields. We encode a given equality
 * prefix by using a combination of distinct scan (for the initial equality prefixes) or a regular
 * index scans (for the last equality prefix). On a high level, we iterate over the distinct
 * combinations of index field values referred to by the equality prefix using a combination of
 * spool producer and consumer nodes. Each unique combination of index field values is then passed
 * onto the next equality prefix which uses it to constrain its own distinct scan, or in the case of
 * the last equality prefix, the final index scan. For a given equality prefix, there is an
 * associated compound interval expression on the referenced fields. The compound interval is a
 * boolean expression consisting of conjunctions and disjunctions, which on a high-level are encoded
 * as index intersection and index union.
 *
 * A few examples:
 *   1. Compound index on {a, b}. Predicates a = 1 and b > 2, translating into one equality prefix
 * with a compound interval ({1, 2}, {1, MaxKey}). The resulting plan is a simple index scan:
 *
 *     IxScan(({1, 2}, {1, MaxKey})).
 *
 *   2. Compound index on {a, b}. Predicates (a = 1 or a = 3) and b > 2. We have one equality prefix
 * with a compound interval ({1, 2}, {1, MaxKey}) U ({3, 2}, {3, MaxKey}). We encode the plan as:
 *
 *   Union
 *       IxScan({1, 2}, {1, MaxKey})
 *       IxScan({3, 2}, {3, MaxKey})
 *
 *   3. Compound index on {a, b}. Predicates a > 1 and b > 2. We now have two equality prefixes. The
 * first equality prefix answered using the distinct scan sub-plan generated to produce distinct
 * values for "a" in the range (1, MaxKey]. The distinct value stream is correlated and made
 * available to the inner index scan which satisfies "b > 2".
 *
 *   NLJ (correlated = a)
 *       DistinctScan(a, (1, MaxKey])
 *       IxScan(({a, 2}, {a, MaxKey}))
 *
 *  Observe the first part of the plan (the outer side of the first correlated join) iterates over
 * the distinct values of the field "a" using a combination of spooling and inner index scan with a
 * variable bound on the current value of "a". The last index scan effectively
 * receives a stream of unique values of "a" and delivers values which also satisfy b > 2.
 *
 *  4. Compound index on {a, b}. Predicates a in [1, 2], b in [3, 4]. We have two equality prefix
 * and we generate the following plan.
 *
 *  Union
 *      NLJ (correlated = a)
 *          DistinctScan(a, [1, 1])
 *          Union
 *              IxScan([{a, 3}, {a, 3}])
 *              IxScan([{a, 4}, {a, 4}])
 *      NLJ (correlated = a)
 *          DistinctScan(a, [2, 2])
 *          Union
 *             IxScan([{a, 3}, {a, 3}])
 *             IxScan([{a, 4}, {a, 4}])
 */
class IntervalLowerTransport {
public:
    IntervalLowerTransport(PrefixId& prefixId,
                           const ProjectionName& ridProjName,
                           FieldProjectionMap indexProjectionMap,
                           const std::string& scanDefName,
                           const std::string& indexDefName,
                           SpoolIdGenerator& spoolId,
                           const size_t indexFieldCount,
                           const std::vector<EqualityPrefixEntry>& eqPrefixes,
                           const size_t currentEqPrefixIndex,
                           const std::vector<bool>& reverseOrder,
                           ProjectionNameVector correlatedProjNames,
                           const std::map<size_t, SelectivityType>& indexPredSelMap,
                           const CEType currentGroupCE,
                           const CEType scanGroupCE,
                           const bool useSortedMerge)
        : _prefixId(prefixId),
          _ridProjName(ridProjName),
          _scanDefName(scanDefName),
          _indexDefName(indexDefName),
          _useSortedMerge(useSortedMerge),
          _spoolId(spoolId),
          _indexFieldCount(indexFieldCount),
          _eqPrefixes(eqPrefixes),
          _currentEqPrefixIndex(currentEqPrefixIndex),
          _currentEqPrefix(_eqPrefixes.at(_currentEqPrefixIndex)),
          _reverseOrder(reverseOrder),
          _indexPredSelMap(indexPredSelMap),
          _scanGroupCE(scanGroupCE) {
        // Collect estimates for predicates satisfied with the current equality prefix.
        // TODO: rationalize cardinality estimates: estimate number of unique groups.
        CEType indexCE = currentGroupCE;
        if (!_currentEqPrefix._predPosSet.empty()) {
            std::vector<SelectivityType> currentSels;
            for (const size_t index : _currentEqPrefix._predPosSet) {
                if (const auto it = indexPredSelMap.find(index); it != indexPredSelMap.cend()) {
                    currentSels.push_back(it->second);
                }
            }
            if (!currentSels.empty()) {
                indexCE = scanGroupCE * ce::conjExponentialBackoff(std::move(currentSels));
            }
        }

        const SelectivityType indexSel =
            (scanGroupCE == 0.0) ? SelectivityType{0.0} : (indexCE / _scanGroupCE);

        _paramStack.push_back(
            {indexSel, std::move(indexProjectionMap), std::move(correlatedProjNames)});
    };

    PhysPlanBuilder transport(const CompoundIntervalReqExpr::Atom& node) {
        const auto& params = _paramStack.back();
        const auto& currentFPM = params._fpm;
        const CEType currentCE = _scanGroupCE * params._estimate;
        const size_t startPos = _currentEqPrefix._startPos;
        const bool reverse = _reverseOrder.at(_currentEqPrefixIndex);

        auto interval = node.getExpr();
        const auto& currentCorrelatedProjNames = params._correlatedProjNames;
        // Update interval with current correlations.
        for (size_t i = 0; i < startPos; i++) {
            auto& lowBound = interval.getLowBound().getBound().at(i);
            lowBound = make<Variable>(currentCorrelatedProjNames.at(i));
            interval.getHighBound().getBound().at(i) = lowBound;
        }

        if (_currentEqPrefixIndex + 1 == _eqPrefixes.size()) {
            // If this is the last equality prefix, use the input field projection map.
            PhysPlanBuilder builder;
            builder.make<IndexScanNode>(
                currentCE, currentFPM, _scanDefName, _indexDefName, std::move(interval), reverse);
            return builder;
        }

        FieldProjectionMap nextFPM = currentFPM;
        FieldProjectionMap outerFPM;

        // Set of correlated projections for next equality prefix.
        ProjectionNameSet correlationSet;

        // Inner and outer auxiliary projections used to set-up the inner index scan parameters to
        // support the spool-based distinct scan.
        ProjectionNameVector innerProjNames;
        ProjectionNameVector outerProjNames;
        // Interval and projection map of the inner index scan.
        CompoundIntervalRequirement innerInterval;
        FieldProjectionMap innerFPM;

        const auto addInnerBound = [&](BoundRequirement bound) {
            const size_t size = innerInterval.size();
            if (reverse) {
                BoundRequirement lowBound{false /*inclusive*/,
                                          interval.getLowBound().getBound().at(size)};
                innerInterval.push_back({std::move(lowBound), std::move(bound)});
            } else {
                BoundRequirement highBound{false /*inclusive*/,
                                           interval.getHighBound().getBound().at(size)};
                innerInterval.push_back({std::move(bound), std::move(highBound)});
            }
        };

        const size_t nextStartPos = _eqPrefixes.at(_currentEqPrefixIndex + 1)._startPos;
        for (size_t indexField = 0; indexField < nextStartPos; indexField++) {
            const FieldNameType indexKey{encodeIndexKeyName(indexField)};

            // Restrict next fpm to not require fields up to "nextStartPos". It should require
            // fields only from the next equality prefixes.
            nextFPM._fieldProjections.erase(indexKey);

            // Generate the combined set of correlated projections from the previous and current
            // equality prefixes.
            const ProjectionName& correlatedProjName = currentCorrelatedProjNames.at(indexField);
            correlationSet.insert(correlatedProjName);

            if (indexField < startPos) {
                // The predicates referring to correlated projections from the previous prefixes
                // are converted to equalities over the distinct set of values.
                addInnerBound({false /*inclusive*/, make<Variable>(correlatedProjName)});
            } else {
                // Use the correlated projections of the current prefix as outer projections.
                outerProjNames.push_back(correlatedProjName);
                outerFPM._fieldProjections.emplace(indexKey, correlatedProjName);

                // For each of the outer projections, generate a set of corresponding inner
                // projections to use for the spool consumer and bounds for the inner index scan.
                auto innerProjName = _prefixId.getNextId("rinInner");
                innerProjNames.push_back(innerProjName);

                addInnerBound({false /*inclusive*/, make<Variable>(std::move(innerProjName))});

                innerFPM._fieldProjections.emplace(indexKey, correlatedProjName);
            }
        }
        while (innerInterval.size() < _indexFieldCount) {
            // Pad the remaining fields in the inner interval.
            addInnerBound(reverse ? BoundRequirement::makeMinusInf()
                                  : BoundRequirement::makePlusInf());
        }

        // Recursively generate a plan to encode the intervals of the subsequent prefixes.
        auto remaining = lowerEqPrefixes(_prefixId,
                                         _ridProjName,
                                         std::move(nextFPM),
                                         _scanDefName,
                                         _indexDefName,
                                         _spoolId,
                                         _indexFieldCount,
                                         _eqPrefixes,
                                         _currentEqPrefixIndex + 1,
                                         _reverseOrder,
                                         currentCorrelatedProjNames,
                                         _indexPredSelMap,
                                         currentCE,
                                         _scanGroupCE,
                                         _useSortedMerge);

        auto result = generateDistinctScan(_scanDefName,
                                           _indexDefName,
                                           reverse,
                                           currentCE,
                                           _spoolId.generate(),
                                           std::move(outerProjNames),
                                           std::move(outerFPM),
                                           std::move(interval),
                                           std::move(innerProjNames),
                                           std::move(innerFPM),
                                           std::move(innerInterval));

        result.make<NestedLoopJoinNode>(currentCE,
                                        JoinType::Inner,
                                        std::move(correlationSet),
                                        Constant::boolean(true),
                                        std::move(result._node),
                                        std::move(remaining._node));
        result.merge(remaining);

        return result;
    }

    template <bool isConjunction>
    void prepare(const size_t childCount) {
        const auto& params = _paramStack.back();

        SelectivityType childSel{1.0};
        if (childCount > 0) {
            // Here we are assuming that children in each conjunction and disjunction contribute
            // equally and independently to the parent's selectivity.
            // TODO: consider estimates per individual interval.

            const SelectivityType parentSel = params._estimate;
            const double childCountInv = 1.0 / childCount;
            if constexpr (isConjunction) {
                childSel = {(parentSel == 0.0) ? SelectivityType{0.0}
                                               : parentSel.pow(childCountInv)};
            } else {
                childSel = parentSel * childCountInv;
            }
        }

        FieldProjectionMap childMap = params._fpm;
        ProjectionNameVector correlatedProjNames = params._correlatedProjNames;
        if (childCount > 1) {
            if (!childMap._ridProjection) {
                childMap._ridProjection = _ridProjName;
            }

            // For projections we require, introduce temporary projections to allow us to union or
            // intersect. Also update the current correlations.
            auto& childFields = childMap._fieldProjections;
            for (size_t indexField = 0; indexField < _indexFieldCount; indexField++) {
                const FieldNameType indexKey{encodeIndexKeyName(indexField)};
                if (auto it = childFields.find(indexKey); it != childFields.end()) {
                    const auto tempProjName =
                        _prefixId.getNextId(isConjunction ? "conjunction" : "disjunction");
                    it->second = tempProjName;
                    if (indexField < correlatedProjNames.size()) {
                        correlatedProjNames.at(indexField) = std::move(tempProjName);
                    }
                }
            }
        }

        _paramStack.push_back({childSel, std::move(childMap), std::move(correlatedProjNames)});
    }

    void prepare(const CompoundIntervalReqExpr::Conjunction& node) {
        prepare<true /*isConjunction*/>(node.nodes().size());
    }

    template <bool isIntersect>
    PhysPlanBuilder implement(std::vector<PhysPlanBuilder> inputs) {
        auto params = std::move(_paramStack.back());
        _paramStack.pop_back();
        auto& prevParams = _paramStack.back();

        const CEType ce = _scanGroupCE * params._estimate;
        auto innerMap = std::move(params._fpm);
        auto outerMap = prevParams._fpm;

        const size_t inputSize = inputs.size();
        if (inputSize == 1) {
            return std::move(inputs.front());
        }

        if (_useSortedMerge) {
            invariant(!isIntersect);
            PhysPlanBuilder result;
            ABTVector inputABTs;
            for (auto& input : inputs) {
                inputABTs.push_back(std::move(input._node));
                result.merge(input);
            }
            // If we're lowering a disjunction and only have equality intervals, use a SortedMerge
            // instead of a Union because the child streams will be sorted. Only applies when
            // sorting on RID only.
            result.make<SortedMergeNode>(
                ce,
                properties::CollationRequirement({{_ridProjName, CollationOp::Ascending}}),
                std::move(inputABTs));
            return result;
        }

        // The input projections names we will be combining from both sides.
        ProjectionNameVector unionProjectionNames;
        // Projection names which will be the result of the combination (intersect or union).
        ProjectionNameVector outerProjNames;
        // Agg expressions used to combine the unioned projections.
        ABTVector aggExpressions;

        unionProjectionNames.push_back(_ridProjName);
        for (const auto& [fieldName, outerProjName] : outerMap._fieldProjections) {
            outerProjNames.push_back(outerProjName);

            const auto& innerProjName = innerMap._fieldProjections.at(fieldName);
            unionProjectionNames.push_back(innerProjName);
            aggExpressions.emplace_back(
                make<FunctionCall>("$first", makeSeq(make<Variable>(innerProjName))));
        }
        ProjectionNameVector aggProjectionNames = outerProjNames;

        boost::optional<ProjectionName> sideSetProjectionName;
        if constexpr (isIntersect) {
            const ProjectionName sideIdProjectionName = _prefixId.getNextId("sideId");
            unionProjectionNames.push_back(sideIdProjectionName);
            sideSetProjectionName = _prefixId.getNextId("sides");

            for (size_t index = 0; index < inputSize; index++) {
                PhysPlanBuilder& input = inputs.at(index);
                // Not relevant for cost.
                input.make<EvaluationNode>(CEType{0.0},
                                           sideIdProjectionName,
                                           Constant::int64(index),
                                           std::move(input._node));
            }

            aggExpressions.emplace_back(
                make<FunctionCall>("$addToSet", makeSeq(make<Variable>(sideIdProjectionName))));
            aggProjectionNames.push_back(*sideSetProjectionName);
        }

        PhysPlanBuilder result;
        {
            ABTVector inputABTs;
            for (auto& input : inputs) {
                inputABTs.push_back(std::move(input._node));
                result.merge(input);
            }
            result.make<UnionNode>(ce, std::move(unionProjectionNames), std::move(inputABTs));
        }

        result.make<GroupByNode>(ce,
                                 ProjectionNameVector{_ridProjName},
                                 std::move(aggProjectionNames),
                                 std::move(aggExpressions),
                                 std::move(result._node));

        if constexpr (isIntersect) {
            result.make<FilterNode>(
                ce,
                make<EvalFilter>(
                    make<PathCompare>(Operations::Eq, Constant::int64(inputSize)),
                    make<FunctionCall>("getArraySize",
                                       makeSeq(make<Variable>(*sideSetProjectionName)))),
                std::move(result._node));
        } else if (!outerMap._ridProjection && !outerProjNames.empty()) {
            // Prevent rid projection from leaking out if we do not require it, and also auxiliary
            // left and right side projections.
            restrictProjections(std::move(outerProjNames), ce, result);
        }

        return result;
    }

    PhysPlanBuilder transport(const CompoundIntervalReqExpr::Conjunction& node,
                              std::vector<PhysPlanBuilder> childResults) {
        return implement<true /*isIntersect*/>(std::move(childResults));
    }

    void prepare(const CompoundIntervalReqExpr::Disjunction& node) {
        prepare<false /*isConjunction*/>(node.nodes().size());
    }

    PhysPlanBuilder transport(const CompoundIntervalReqExpr::Disjunction& node,
                              std::vector<PhysPlanBuilder> childResults) {
        return implement<false /*isIntersect*/>(std::move(childResults));
    }

    PhysPlanBuilder lower(const CompoundIntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }

private:
    PrefixId& _prefixId;
    const ProjectionName& _ridProjName;
    const std::string& _scanDefName;
    const std::string& _indexDefName;
    const bool _useSortedMerge;

    // Equality-prefix and related.
    SpoolIdGenerator& _spoolId;
    const size_t _indexFieldCount;
    const std::vector<EqualityPrefixEntry>& _eqPrefixes;
    const size_t _currentEqPrefixIndex;
    const EqualityPrefixEntry& _currentEqPrefix;
    const std::vector<bool>& _reverseOrder;
    const std::map<size_t, SelectivityType>& _indexPredSelMap;

    const CEType _scanGroupCE;

    // Stack which is used to support carrying and updating parameters across Conjunction and
    // Disjunction nodes.
    struct StackEntry {
        SelectivityType _estimate;
        FieldProjectionMap _fpm;
        ProjectionNameVector _correlatedProjNames;
    };
    std::vector<StackEntry> _paramStack;
};

PhysPlanBuilder lowerEqPrefixes(PrefixId& prefixId,
                                const ProjectionName& ridProjName,
                                FieldProjectionMap indexProjectionMap,
                                const std::string& scanDefName,
                                const std::string& indexDefName,
                                SpoolIdGenerator& spoolId,
                                const size_t indexFieldCount,
                                const std::vector<EqualityPrefixEntry>& eqPrefixes,
                                const size_t eqPrefixIndex,
                                const std::vector<bool>& reverseOrder,
                                ProjectionNameVector correlatedProjNames,
                                const std::map<size_t, SelectivityType>& indexPredSelMap,
                                const CEType indexCE,
                                const CEType scanGroupCE,
                                const bool useSortedMerge) {
    IntervalLowerTransport lowerTransport(prefixId,
                                          ridProjName,
                                          std::move(indexProjectionMap),
                                          scanDefName,
                                          indexDefName,
                                          spoolId,
                                          indexFieldCount,
                                          eqPrefixes,
                                          eqPrefixIndex,
                                          reverseOrder,
                                          correlatedProjNames,
                                          indexPredSelMap,
                                          indexCE,
                                          scanGroupCE,
                                          useSortedMerge);
    return lowerTransport.lower(eqPrefixes.at(eqPrefixIndex)._interval);
}

bool hasProperIntervals(const PSRExpr::Node& reqs) {
    // Compute if this node has any proper (not fully open) intervals.
    bool hasProperIntervals = false;
    PSRExpr::visitAnyShape(reqs, [&](const PartialSchemaEntry& e) {
        hasProperIntervals |= !isIntervalReqFullyOpenDNF(e.second.getIntervals());
    });
    return hasProperIntervals;
}

}  // namespace mongo::optimizer
