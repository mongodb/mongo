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

#include "mongo/db/query/optimizer/cascades/implementers.h"

#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"
#include "mongo/db/query/optimizer/utils/reftracker_utils.h"


namespace mongo::optimizer::cascades {
using namespace properties;

template <class P, class T>
static bool propertyAffectsProjections(const PhysProps& props, const T& projections) {
    if (!hasProperty<P>(props)) {
        return false;
    }

    const ProjectionNameSet& propProjections =
        getPropertyConst<P>(props).getAffectedProjectionNames();
    for (const ProjectionName& projectionName : projections) {
        if (propProjections.find(projectionName) != propProjections.cend()) {
            return true;
        }
    }

    return false;
}

template <class P>
static bool propertyAffectsProjection(const PhysProps& props,
                                      const ProjectionName& projectionName) {
    return propertyAffectsProjections<P>(props, ProjectionNameVector{projectionName});
}

// Checks that all leaves of the expression are equalities. This would indicate that we could use
// SortedMerge and MergeJoin to produce a stream of sorted RIDs, allowing us to potentially
// deduplicate with a streaming Unique.
static bool canReturnSortedOutput(const CompoundIntervalReqExpr::Node& intervals) {
    bool canBeSorted = true;
    // TODO SERVER-73828 this pattern could use early return.
    CompoundIntervalReqExpr::visitDisjuncts(
        intervals, [&](const CompoundIntervalReqExpr::Node& conj, size_t) {
            CompoundIntervalReqExpr::visitConjuncts(
                conj, [&](const CompoundIntervalReqExpr::Node& atom, size_t i) {
                    if (i > 0) {
                        canBeSorted = false;
                    } else {
                        CompoundIntervalReqExpr::visitAtom(
                            atom, [&](const CompoundIntervalRequirement& req) {
                                if (!req.isEquality()) {
                                    canBeSorted = false;
                                }
                            });
                    }
                });
        });
    // We shouldn't use a SortedMerge for a singleton disjunction, because with one child there is
    // nothing to sort-merge.
    return canBeSorted && !CompoundIntervalReqExpr::isSingletonDisjunction(intervals);
}

/**
 * Implement physical nodes based on existing logical nodes.
 */
class ImplementationVisitor {
public:
    void operator()(const ABT& /*n*/, const ScanNode& node) {
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // Cannot satisfy limit-skip.
            return;
        }
        if (hasProperty<CollationRequirement>(_physProps)) {
            // Regular scan cannot satisfy any collation requirement.
            // TODO: consider rid?
            return;
        }

        const auto& indexReq = getPropertyConst<IndexingRequirement>(_physProps);
        const IndexReqTarget indexReqTarget = indexReq.getIndexReqTarget();
        switch (indexReqTarget) {
            case IndexReqTarget::Index:
                // At this point cannot only satisfy index-only.
                return;

            case IndexReqTarget::Seek:
                if (_hints._disableIndexes == DisableIndexOptions::DisableAll) {
                    return;
                }
                // Fall through to code below.
                break;

            case IndexReqTarget::Complete:
                if (_hints._disableScan) {
                    return;
                }
                // Fall through to code below.
                break;

            default:
                MONGO_UNREACHABLE;
        }

        // Handle complete indexing requirement.
        bool canUseParallelScan = false;
        if (!distributionsCompatible(
                indexReqTarget,
                _metadata._scanDefs.at(node.getScanDefName()).getDistributionAndPaths(),
                node.getProjectionName(),
                _logicalProps,
                {},
                canUseParallelScan)) {
            return;
        }

        const auto& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();
        const ProjectionName& ridProjName = _ridProjections.at(node.getScanDefName());

        FieldProjectionMap fieldProjectionMap;
        for (const ProjectionName& required : requiredProjections.getVector()) {
            if (required == node.getProjectionName()) {
                fieldProjectionMap._rootProjection = node.getProjectionName();
            } else if (required == ridProjName) {
                fieldProjectionMap._ridProjection = ridProjName;
            } else {
                // Regular scan node can satisfy only using its root or rid projections (not
                // fields).
                return;
            }
        }

        if (indexReqTarget == IndexReqTarget::Seek) {
            PhysPlanBuilder builder;
            // If optimizing a Seek, override CE to 1.0.
            builder.make<SeekNode>(
                CEType{1.0}, ridProjName, std::move(fieldProjectionMap), node.getScanDefName());
            builder.make<LimitSkipNode>(
                CEType{1.0}, LimitSkipRequirement{1, 0}, std::move(builder._node));

            optimizeChildrenNoAssert(_queue,
                                     kDefaultPriority,
                                     PhysicalRewriteType::Seek,
                                     std::move(builder._node),
                                     {},
                                     std::move(builder._nodeCEMap));
        } else {
            ABT physicalScan = make<PhysicalScanNode>(
                std::move(fieldProjectionMap), node.getScanDefName(), canUseParallelScan);
            optimizeChild<PhysicalScanNode, PhysicalRewriteType::PhysicalScan>(
                _queue, kDefaultPriority, std::move(physicalScan));
        }
    }

    void operator()(const ABT& n, const ValueScanNode& node) {
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // Cannot satisfy limit-skip.
            return;
        }
        if (hasProperty<CollationRequirement>(_physProps)) {
            // Cannot satisfy any collation requirement.
            return;
        }

        const auto& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();

        boost::optional<ProjectionName> ridProjName;
        bool needsRID = false;
        if (hasProperty<IndexingAvailability>(_logicalProps)) {
            ridProjName = _ridProjections.at(
                getPropertyConst<IndexingAvailability>(_logicalProps).getScanDefName());
            needsRID = requiredProjections.find(*ridProjName).has_value();
        }
        if (needsRID && !node.getHasRID()) {
            // We cannot provide RID.
            return;
        }

        PhysPlanBuilder builder;
        if (node.getArraySize() == 0) {
            builder.make<CoScanNode>(CEType{0.0});
            builder.make<LimitSkipNode>(
                CEType{0.0}, LimitSkipRequirement{0, 0}, std::move(builder._node));

            for (const ProjectionName& boundProjName : node.binder().names()) {
                if (requiredProjections.find(boundProjName)) {
                    builder.make<EvaluationNode>(
                        CEType{0.0}, boundProjName, Constant::nothing(), std::move(builder._node));
                }
            }
            if (needsRID) {
                builder.make<EvaluationNode>(CEType{0.0},
                                             std::move(*ridProjName),
                                             Constant::nothing(),
                                             std::move(builder._node));
            }
        } else {
            builder.make<CoScanNode>(CEType{1.0});
            builder.make<LimitSkipNode>(
                CEType{1.0}, LimitSkipRequirement{1, 0}, std::move(builder._node));

            const ProjectionName valueScanProj{_prefixId.getNextId("valueScan")};
            builder.make<EvaluationNode>(
                CEType{1.0}, valueScanProj, node.getValueArray(), std::move(builder._node));

            // Unwind the combined array constant and pick an element for each required projection
            // in sequence.
            builder.make<UnwindNode>(CEType{1.0},
                                     valueScanProj,
                                     _prefixId.getNextId("valueScanPid"),
                                     false /*retainNonArrays*/,
                                     std::move(builder._node));

            const auto getElementFn = [&valueScanProj](const size_t index) {
                return make<FunctionCall>(
                    "getElement", makeSeq(make<Variable>(valueScanProj), Constant::int32(index)));
            };

            // Iterate over the bound projections here as opposed to the required projections, since
            // the array elements are ordered accordingly. Skip over the first element (this is the
            // row id).
            const ProjectionNameVector& boundProjNames = node.binder().names();
            const CEType arraySize{static_cast<double>(node.getArraySize())};
            for (size_t i = 0; i < boundProjNames.size(); i++) {
                const ProjectionName& boundProjName = boundProjNames.at(i);
                if (requiredProjections.find(boundProjName)) {
                    builder.make<EvaluationNode>(arraySize,
                                                 boundProjName,
                                                 getElementFn(i + (node.getHasRID() ? 1 : 0)),
                                                 std::move(builder._node));
                }
            }

            if (needsRID) {
                // Obtain row id from first element of the array.
                builder.make<EvaluationNode>(
                    arraySize, std::move(*ridProjName), getElementFn(0), std::move(builder._node));
            }
        }

        optimizeChildrenNoAssert(_queue,
                                 kDefaultPriority,
                                 PhysicalRewriteType::ValueScan,
                                 std::move(builder._node),
                                 {},
                                 std::move(builder._nodeCEMap));
    }

    void operator()(const ABT& /*n*/, const MemoLogicalDelegatorNode& /*node*/) {
        uasserted(6624041,
                  "Must not have logical delegator nodes in the list of the logical nodes");
    }

    void operator()(const ABT& n, const FilterNode& node) {
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // We cannot satisfy here.
            return;
        }

        ProjectionNameSet references = collectVariableReferences(n);
        if (checkIntroducesScanProjectionUnderIndexOnly(references)) {
            // Reject if under indexing requirements and now we introduce dependence on scan
            // projection.
            return;
        }

        PhysProps newProps = _physProps;
        // Add projections we depend on to the requirement.
        addProjectionsToProperties(newProps, std::move(references));
        getProperty<DistributionRequirement>(newProps).setDisableExchanges(true);

        ABT physicalFilter = n;
        optimizeChild<FilterNode, PhysicalRewriteType::Filter>(
            _queue, kDefaultPriority, std::move(physicalFilter), std::move(newProps));
    }

    void operator()(const ABT& n, const EvaluationNode& node) {
        const ProjectionName& projectionName = node.getProjectionName();

        if (const auto* varPtr = node.getProjection().cast<Variable>(); varPtr != nullptr) {
            // Special case of evaluation node: rebinds to a different variable.
            const ProjectionName& newProjName = varPtr->name();
            PhysProps newProps = _physProps;

            {
                // Update required projections.
                auto& reqProjections =
                    getProperty<ProjectionRequirement>(newProps).getProjections();
                reqProjections.erase(projectionName);
                reqProjections.emplace_back(newProjName);
            }

            if (hasProperty<CollationRequirement>(newProps)) {
                // Update the collation specification to use the input variable.
                auto& collationReq = getProperty<CollationRequirement>(newProps);
                for (auto& [projName, op] : collationReq.getCollationSpec()) {
                    if (projName == projectionName) {
                        projName = newProjName;
                    }
                }
            }

            {
                // Update the distribution specification to use the input variable;
                auto& distribReq = getProperty<DistributionRequirement>(newProps);
                for (auto& projName : distribReq.getDistributionAndProjections()._projectionNames) {
                    if (projName == projectionName) {
                        projName = newProjName;
                    }
                }
            }

            ABT physicalEval = n;
            optimizeChild<EvaluationNode, PhysicalRewriteType::RenameProjection>(
                _queue, kDefaultPriority, std::move(physicalEval), std::move(newProps));
            return;
        }

        if (propertyAffectsProjection<DistributionRequirement>(_physProps, projectionName)) {
            // We cannot satisfy distribution on the projection we output.
            return;
        }
        if (propertyAffectsProjection<CollationRequirement>(_physProps, projectionName)) {
            // In general, we cannot satisfy collation on the projection we output.
            // TODO consider x = y+1, we can propagate the collation requirement from x to y.
            return;
        }
        if (!propertyAffectsProjection<ProjectionRequirement>(_physProps, projectionName)) {
            // We do not require the projection. Do not place a physical evaluation node and
            // continue optimizing the child.
            optimizeUnderNewProperties<PhysicalRewriteType::EvaluationPassthrough>(
                _queue, kDefaultPriority, node.getChild(), _physProps);
            return;
        }

        // Remove our projection from requirement, and add projections we depend on to the
        // requirement.
        PhysProps newProps = _physProps;

        ProjectionNameSet references = collectVariableReferences(n);
        if (checkIntroducesScanProjectionUnderIndexOnly(references)) {
            // Reject if under indexing requirements and now we introduce dependence on scan
            // projection.
            return;
        }

        addRemoveProjectionsToProperties(
            newProps, std::move(references), ProjectionNameVector{projectionName});
        getProperty<DistributionRequirement>(newProps).setDisableExchanges(true);

        ABT physicalEval = n;
        optimizeChild<EvaluationNode, PhysicalRewriteType::Evaluation>(
            _queue, kDefaultPriority, std::move(physicalEval), std::move(newProps));
    }

    void operator()(const ABT& n, const SargableNode& node) {
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // Cannot satisfy limit-skip.
            return;
        }

        const IndexingAvailability& indexingAvailability =
            getPropertyConst<IndexingAvailability>(_logicalProps);
        if (node.getChild().cast<MemoLogicalDelegatorNode>()->getGroupId() !=
            indexingAvailability.getScanGroupId()) {
            // To optimize a sargable predicate, we must have the scan group as a child.
            return;
        }

        const std::string& scanDefName = indexingAvailability.getScanDefName();
        const auto& scanDef = _metadata._scanDefs.at(scanDefName);


        // We do not check indexDefs to be empty here. We want to allow evaluations to be covered
        // via a physical scan even in the absence of indexes.

        const IndexingRequirement& requirements = getPropertyConst<IndexingRequirement>(_physProps);
        const CandidateIndexes& candidateIndexes = node.getCandidateIndexes();
        const IndexReqTarget indexReqTarget = requirements.getIndexReqTarget();
        const PartialSchemaRequirements& reqMap = node.getReqMap();

        switch (indexReqTarget) {
            case IndexReqTarget::Complete:
                if (_hints._disableScan) {
                    return;
                }
                if (_hints._forceIndexScanForPredicates && hasProperIntervals(reqMap.getRoot())) {
                    return;
                }
                break;

            case IndexReqTarget::Index:
                if (candidateIndexes.empty()) {
                    return;
                }
                [[fallthrough]];

            case IndexReqTarget::Seek:
                if (_hints._disableIndexes == DisableIndexOptions::DisableAll) {
                    return;
                }
                break;

            default:
                MONGO_UNREACHABLE;
        }

        if (indexReqTarget != IndexReqTarget::Index &&
            hasProperty<CollationRequirement>(_physProps)) {
            // PhysicalScan or Seek cannot satisfy any collation requirement.
            // TODO: consider rid?
            return;
        }

        const ProjectionName& scanProjectionName = indexingAvailability.getScanProjection();

        // We can only satisfy partial schema requirements using our root projection.
        {
            bool anyNonRoot = false;
            PSRExpr::visitAnyShape(reqMap.getRoot(), [&](const PartialSchemaEntry& e) {
                if (e.first._projectionName != scanProjectionName) {
                    anyNonRoot = true;
                }
            });
            if (anyNonRoot) {
                return;
            }
        }

        const auto& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();
        const ProjectionName& ridProjName = _ridProjections.at(scanDefName);

        bool needsRID = false;
        bool requiresRootProjection = false;
        {
            auto projectionsLeftToSatisfy = requiredProjections;
            needsRID = projectionsLeftToSatisfy.erase(ridProjName);
            if (indexReqTarget != IndexReqTarget::Index) {
                // Deliver root projection if required.
                requiresRootProjection = projectionsLeftToSatisfy.erase(scanProjectionName);
            }

            for (const auto& [key, boundProjName] : getBoundProjections(reqMap)) {
                projectionsLeftToSatisfy.erase(boundProjName);
            }
            if (!projectionsLeftToSatisfy.getVector().empty()) {
                // Unknown projections remain. Reject.
                return;
            }
        }

        const GroupIdType scanGroupId = indexingAvailability.getScanGroupId();
        const LogicalProps& scanLogicalProps = _memo.getLogicalProps(scanGroupId);
        const CEType scanGroupCE =
            getPropertyConst<CardinalityEstimate>(scanLogicalProps).getEstimate();

        const auto& ceProperty = getPropertyConst<CardinalityEstimate>(_logicalProps);
        const CEType currentGroupCE = ceProperty.getEstimate();
        const PartialSchemaKeyCE& partialSchemaKeyCE = ceProperty.getPartialSchemaKeyCE();

        if (indexReqTarget == IndexReqTarget::Index) {
            ProjectionCollationSpec requiredCollation;
            if (hasProperty<CollationRequirement>(_physProps)) {
                requiredCollation =
                    getPropertyConst<CollationRequirement>(_physProps).getCollationSpec();
            }

            const auto& satisfiedPartialIndexes =
                getPropertyConst<IndexingAvailability>(
                    _memo.getLogicalProps(requirements.getSatisfiedPartialIndexesGroupId()))
                    .getSatisfiedPartialIndexes();

            // Consider all candidate indexes, and check if they satisfy the collation and
            // distribution requirements.
            for (const auto& candidateIndexEntry : node.getCandidateIndexes()) {
                const auto& indexDefName = candidateIndexEntry._indexDefName;
                const auto& indexDef = scanDef.getIndexDefs().at(indexDefName);

                if (!indexDef.getPartialReqMap().isNoop() &&
                    (_hints._disableIndexes == DisableIndexOptions::DisablePartialOnly ||
                     satisfiedPartialIndexes.count(indexDefName) == 0)) {
                    // Consider only indexes for which we satisfy partial requirements.
                    continue;
                }

                {
                    bool canUseParallelScanUnused = false;
                    if (!distributionsCompatible(IndexReqTarget::Index,
                                                 indexDef.getDistributionAndPaths(),
                                                 scanProjectionName,
                                                 scanLogicalProps,
                                                 reqMap,
                                                 canUseParallelScanUnused)) {
                        return;
                    }
                }

                const auto availableDirections =
                    indexSatisfiesCollation(indexDef.getCollationSpec(),
                                            candidateIndexEntry,
                                            requiredCollation,
                                            ridProjName);
                if (!availableDirections) {
                    // Failed to satisfy collation requirement.
                    continue;
                }

                auto indexProjectionMap = candidateIndexEntry._fieldProjectionMap;
                auto residualReqs = candidateIndexEntry._residualRequirements;
                removeRedundantResidualPredicates(
                    requiredProjections, residualReqs, indexProjectionMap);

                // Compute the selectivities of predicates covered by index bounds and by residual
                // predicates.
                std::vector<SelectivityType> indexPredSels;
                std::map<size_t, SelectivityType> indexPredSelMap;
                {
                    std::set<size_t> residIndexes;
                    if (residualReqs) {
                        ResidualRequirements::visitDNF(
                            *residualReqs, [&](const ResidualRequirement& residReq) {
                                residIndexes.emplace(residReq._entryIndex);
                            });
                    }

                    // Compute the selectivity of the indexed requirements by excluding reqs tracked
                    // in 'residIndexes'.
                    if (scanGroupCE > 0.0) {
                        size_t entryIndex = 0;
                        std::vector<SelectivityType> atomSels;
                        std::vector<SelectivityType> conjuctionSels;
                        PSRExpr::visitDisjuncts(
                            reqMap.getRoot(), [&](const PSRExpr::Node& child, const size_t) {
                                atomSels.clear();

                                PSRExpr::visitConjuncts(
                                    child, [&](const PSRExpr::Node& atom, const size_t) {
                                        PSRExpr::visitAtom(
                                            atom, [&](const PartialSchemaEntry& entry) {
                                                if (residIndexes.count(entryIndex) == 0) {
                                                    const SelectivityType sel =
                                                        partialSchemaKeyCE.at(entryIndex).second /
                                                        scanGroupCE;
                                                    atomSels.push_back(sel);
                                                    indexPredSelMap.emplace(entryIndex, sel);
                                                }
                                                entryIndex++;
                                            });
                                    });

                                if (!atomSels.empty()) {
                                    conjuctionSels.push_back(ce::conjExponentialBackoff(atomSels));
                                }
                            });

                        if (!conjuctionSels.empty()) {
                            indexPredSels.push_back(ce::disjExponentialBackoff(conjuctionSels));
                        }
                    }
                }

                /**
                 * The following logic determines whether we need a unique stage to deduplicate the
                 * RIDs returned by the query. If the caller doesn't need unique RIDs then you don't
                 * need to provide them. if the index is non-multikey there are no duplicates. If
                 * there is more than one interval in the BoolExpr, then the helper method that is
                 * called currently creates Groupby+Union which will already deduplicate. Finally,
                 * if there is only one interval, but it's a point interval, then the index won't
                 * have duplicates.
                 *
                 * If there is more than one equality prefix, and any of them needs a unique stage,
                 * we add one after we translate the combined eqPrefix plan.
                 */
                const auto& eqPrefixes = candidateIndexEntry._eqPrefixes;

                // For now we only use SortedMerge for one equality prefix. We also check the field
                // projections for information about whether we need a GroupBy to perform
                // aggregations, in which case a unique is unnecessary and therefore a SortedMerge
                // is not needed. `canReturnSortedOutput` tells us if we have more than one
                // predicate, and if all of these predicates are equalities.
                const bool usedSortedMerge = eqPrefixes.size() == 1 &&
                    canReturnSortedOutput(eqPrefixes.front()._interval) &&
                    indexProjectionMap._fieldProjections.empty();
                bool needsUniqueStage = indexDef.isMultiKey() && requirements.getDedupRID();

                // If we have decided to use SortedMerge and we need a unique stage, skip this check
                // because we will not produce GroupBys to dedup. We always need the Unique.
                if (needsUniqueStage && !usedSortedMerge) {
                    // TODO: consider pre-computing in "computeCandidateIndexes()".
                    bool simpleRanges = false;
                    for (const auto& eqPrefix : eqPrefixes) {
                        if (isSimpleRange(eqPrefix._interval)) {
                            simpleRanges = true;
                            break;
                        }
                    }
                    if (!simpleRanges) {
                        needsUniqueStage = false;
                    }
                }

                if (needsRID || needsUniqueStage) {
                    indexProjectionMap._ridProjection = ridProjName;
                }

                // Compute reversing per equality prefix.
                std::vector<bool> reverseOrder;
                for (const auto& entry : *availableDirections) {
                    reverseOrder.push_back(!entry._forward);
                }
                invariant(eqPrefixes.size() == reverseOrder.size());

                auto builder = lowerEqPrefixes(_prefixId,
                                               ridProjName,
                                               std::move(indexProjectionMap),
                                               scanDefName,
                                               indexDefName,
                                               _spoolId,
                                               indexDef.getCollationSpec().size(),
                                               eqPrefixes,
                                               0 /*currentEqPrefixIndex*/,
                                               reverseOrder,
                                               candidateIndexEntry._correlatedProjNames.getVector(),
                                               std::move(indexPredSelMap),
                                               currentGroupCE,
                                               scanGroupCE,
                                               usedSortedMerge);

                if (residualReqs) {
                    auto reqsWithCE = createResidualReqsWithCE(*residualReqs, partialSchemaKeyCE);
                    lowerPartialSchemaRequirements(scanGroupCE,
                                                   scanGroupCE,
                                                   std::move(indexPredSels),
                                                   std::move(reqsWithCE),
                                                   _pathToInterval,
                                                   builder);
                }

                if (needsUniqueStage) {
                    // Insert unique stage if we need to, after the residual requirements.
                    builder.make<UniqueNode>(currentGroupCE,
                                             ProjectionNameVector{ridProjName},
                                             std::move(builder._node));
                }

                optimizeChildrenNoAssert(_queue,
                                         kDefaultPriority,
                                         PhysicalRewriteType::SargableToIndex,
                                         std::move(builder._node),
                                         {},
                                         std::move(builder._nodeCEMap));
            }
        } else {
            const auto& scanParams = node.getScanParams();
            tassert(6624102, "Empty scan params", scanParams);

            bool canUseParallelScan = false;
            if (!distributionsCompatible(indexReqTarget,
                                         scanDef.getDistributionAndPaths(),
                                         scanProjectionName,
                                         scanLogicalProps,
                                         reqMap,
                                         canUseParallelScan)) {
                return;
            }

            FieldProjectionMap fieldProjectionMap = scanParams->_fieldProjectionMap;
            auto residualReqs = scanParams->_residualRequirements;
            removeRedundantResidualPredicates(
                requiredProjections, residualReqs, fieldProjectionMap);

            if (indexReqTarget == IndexReqTarget::Complete && needsRID) {
                fieldProjectionMap._ridProjection = ridProjName;
            }
            if (requiresRootProjection) {
                fieldProjectionMap._rootProjection = scanProjectionName;
            }

            PhysPlanBuilder builder;
            CEType baseCE{0.0};

            PhysicalRewriteType rule = PhysicalRewriteType::Uninitialized;
            if (indexReqTarget == IndexReqTarget::Complete) {
                baseCE = scanGroupCE;

                // Return a physical scan with field map.
                builder.make<PhysicalScanNode>(
                    baseCE, std::move(fieldProjectionMap), scanDefName, canUseParallelScan);
                rule = PhysicalRewriteType::SargableToPhysicalScan;
            } else {
                baseCE = {1.0};

                // Try Seek with Limit 1.
                builder.make<SeekNode>(
                    baseCE, ridProjName, std::move(fieldProjectionMap), scanDefName);

                builder.make<LimitSkipNode>(
                    baseCE, LimitSkipRequirement{1, 0}, std::move(builder._node));
                rule = PhysicalRewriteType::SargableToSeek;
            }

            if (residualReqs) {
                auto reqsWithCE = createResidualReqsWithCE(*residualReqs, partialSchemaKeyCE);
                lowerPartialSchemaRequirements(scanGroupCE,
                                               baseCE,
                                               {} /*indexPredSels*/,
                                               std::move(reqsWithCE),
                                               _pathToInterval,
                                               builder);
            }

            optimizeChildrenNoAssert(_queue,
                                     kDefaultPriority,
                                     rule,
                                     std::move(builder._node),
                                     {},
                                     std::move(builder._nodeCEMap));
        }
    }

    void operator()(const ABT& /*n*/, const RIDIntersectNode& node) {
        const auto& indexingAvailability = getPropertyConst<IndexingAvailability>(_logicalProps);
        const std::string& scanDefName = indexingAvailability.getScanDefName();
        {
            const auto& scanDef = _metadata._scanDefs.at(scanDefName);
            if (scanDef.getIndexDefs().empty()) {
                // Reject if we do not have any indexes.
                return;
            }
        }
        const auto& ridProjName = _ridProjections.at(scanDefName);

        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // Cannot satisfy limit-skip.
            return;
        }

        const IndexingRequirement& requirements = getPropertyConst<IndexingRequirement>(_physProps);
        const bool dedupRID = requirements.getDedupRID();
        const IndexReqTarget indexReqTarget = requirements.getIndexReqTarget();
        if (indexReqTarget == IndexReqTarget::Seek) {
            return;
        }
        const bool isIndex = indexReqTarget == IndexReqTarget::Index;

        const GroupIdType leftGroupId =
            node.getLeftChild().cast<MemoLogicalDelegatorNode>()->getGroupId();
        const GroupIdType rightGroupId =
            node.getRightChild().cast<MemoLogicalDelegatorNode>()->getGroupId();

        const LogicalProps& leftLogicalProps = _memo.getLogicalProps(leftGroupId);
        const LogicalProps& rightLogicalProps = _memo.getLogicalProps(rightGroupId);

        const bool hasProperIntervalLeft =
            getPropertyConst<IndexingAvailability>(leftLogicalProps).hasProperInterval();
        const bool hasProperIntervalRight =
            getPropertyConst<IndexingAvailability>(rightLogicalProps).hasProperInterval();

        if (isIndex && (!hasProperIntervalLeft || !hasProperIntervalRight)) {
            // We need to have proper intervals on both sides.
            return;
        }

        const auto& distribRequirement = getPropertyConst<DistributionRequirement>(_physProps);
        const auto& distrAndProjections = distribRequirement.getDistributionAndProjections();
        if (isIndex) {
            switch (distrAndProjections._type) {
                case DistributionType::UnknownPartitioning:
                case DistributionType::RoundRobin:
                    // Cannot satisfy unknown or round-robin distributions.
                    return;

                default:
                    break;
            }
        }

        const CEType intersectedCE =
            getPropertyConst<CardinalityEstimate>(_logicalProps).getEstimate();
        const CEType leftCE = getPropertyConst<CardinalityEstimate>(leftLogicalProps).getEstimate();
        const CEType rightCE =
            getPropertyConst<CardinalityEstimate>(rightLogicalProps).getEstimate();
        const ProjectionNameSet& leftProjections =
            getPropertyConst<ProjectionAvailability>(leftLogicalProps).getProjections();
        const ProjectionNameSet& rightProjections =
            getPropertyConst<ProjectionAvailability>(rightLogicalProps).getProjections();

        // Split required projections between inner and outer side.
        ProjectionNameOrderPreservingSet leftChildProjections;
        ProjectionNameOrderPreservingSet rightChildProjections;

        // If we are performing an intersection we need to obtain rids from both sides.
        leftChildProjections.emplace_back(ridProjName);
        if (isIndex) {
            rightChildProjections.emplace_back(ridProjName);
        }

        for (const ProjectionName& projectionName :
             getPropertyConst<ProjectionRequirement>(_physProps).getProjections().getVector()) {
            if (projectionName == ridProjName) {
                continue;
            }

            if (projectionName != node.getScanProjectionName() &&
                leftProjections.count(projectionName) > 0) {
                leftChildProjections.emplace_back(projectionName);
            } else if (rightProjections.count(projectionName) > 0) {
                if (isIndex && projectionName == node.getScanProjectionName()) {
                    return;
                }
                rightChildProjections.emplace_back(projectionName);
            } else {
                uasserted(6624104,
                          "Required projection must appear in either the left or the right child "
                          "projections");
                return;
            }
        }

        ProjectionCollationSpec collationSpec;
        if (hasProperty<CollationRequirement>(_physProps)) {
            collationSpec = getPropertyConst<CollationRequirement>(_physProps).getCollationSpec();
        }

        // Split collation between inner and outer side.
        const CollationSplitResult& collationLeftRightSplit =
            splitCollationSpec(ridProjName, collationSpec, leftProjections, rightProjections);
        const CollationSplitResult& collationRightLeftSplit =
            splitCollationSpec(ridProjName, collationSpec, rightProjections, leftProjections);

        // We are propagating the distribution requirements to both sides.
        PhysProps leftPhysProps = _physProps;
        PhysProps rightPhysProps = _physProps;

        getProperty<DistributionRequirement>(leftPhysProps).setDisableExchanges(false);
        getProperty<DistributionRequirement>(rightPhysProps).setDisableExchanges(false);

        setPropertyOverwrite<IndexingRequirement>(
            leftPhysProps,
            {IndexReqTarget::Index,
             !isIndex && dedupRID,
             requirements.getSatisfiedPartialIndexesGroupId()});
        setPropertyOverwrite<IndexingRequirement>(
            rightPhysProps,
            {isIndex ? IndexReqTarget::Index : IndexReqTarget::Seek,
             !isIndex && dedupRID,
             requirements.getSatisfiedPartialIndexesGroupId()});

        setPropertyOverwrite<ProjectionRequirement>(leftPhysProps, std::move(leftChildProjections));
        setPropertyOverwrite<ProjectionRequirement>(rightPhysProps,
                                                    std::move(rightChildProjections));

        if (!isIndex) {
            // Add repeated execution property to inner side.
            double estimatedRepetitions = hasProperty<RepetitionEstimate>(_physProps)
                ? getPropertyConst<RepetitionEstimate>(_physProps).getEstimate()
                : 1.0;
            estimatedRepetitions *=
                getPropertyConst<CardinalityEstimate>(leftLogicalProps).getEstimate()._value;
            setPropertyOverwrite<RepetitionEstimate>(rightPhysProps,
                                                     RepetitionEstimate{estimatedRepetitions});
        }

        const auto& optimizeFn = [this,
                                  isIndex,
                                  dedupRID,
                                  &indexingAvailability,
                                  &ridProjName,
                                  &collationLeftRightSplit,
                                  &collationRightLeftSplit,
                                  intersectedCE,
                                  leftCE,
                                  rightCE,
                                  &leftPhysProps,
                                  &rightPhysProps,
                                  &node] {
            optimizeRIDIntersect(isIndex,
                                 dedupRID,
                                 indexingAvailability.getEqPredsOnly(),
                                 ridProjName,
                                 collationLeftRightSplit,
                                 collationRightLeftSplit,
                                 intersectedCE,
                                 leftCE,
                                 rightCE,
                                 leftPhysProps,
                                 rightPhysProps,
                                 node.getLeftChild(),
                                 node.getRightChild());
        };

        const auto& leftDistributions =
            getPropertyConst<DistributionAvailability>(leftLogicalProps).getDistributionSet();
        const auto& rightDistributions =
            getPropertyConst<DistributionAvailability>(rightLogicalProps).getDistributionSet();

        const bool leftDistrOK = leftDistributions.count(distrAndProjections) > 0;
        const bool rightDistrOK = rightDistributions.count(distrAndProjections) > 0;
        const bool seekWithRoundRobin = !isIndex &&
            distrAndProjections._type == DistributionType::RoundRobin &&
            rightDistributions.count(DistributionType::UnknownPartitioning) > 0;

        if (leftDistrOK && (rightDistrOK || seekWithRoundRobin)) {
            // If we are not changing the distributions, both the left and right children need to
            // have it available. For example, if optimizing under HashPartitioning on "var1", we
            // need to check that this distribution is available in both child groups. If not, and
            // it is available in one group, we can try replicating the other group (below).
            // If optimizing the seek side specifically, we allow for a RoundRobin distribution
            // which can match a collection with UnknownPartitioning.
            optimizeFn();
        }

        if (!isIndex) {
            // Nothing more to do for Complete target. The index side needs to be collocated with
            // the seek side.
            return;
        }

        // Specifically for index intersection, try propagating the requirement on one
        // side and replicating the other.
        switch (distrAndProjections._type) {
            case DistributionType::HashPartitioning:
            case DistributionType::RangePartitioning: {
                if (leftDistrOK) {
                    setPropertyOverwrite<DistributionRequirement>(leftPhysProps,
                                                                  distribRequirement);
                    setPropertyOverwrite<DistributionRequirement>(
                        rightPhysProps, DistributionRequirement{DistributionType::Replicated});
                    optimizeFn();
                }

                if (rightDistrOK) {
                    setPropertyOverwrite<DistributionRequirement>(
                        leftPhysProps, DistributionRequirement{DistributionType::Replicated});
                    setPropertyOverwrite<DistributionRequirement>(rightPhysProps,
                                                                  distribRequirement);
                    optimizeFn();
                }
                break;
            }

            default:
                break;
        }
    }

    void operator()(const ABT& /*n*/, const RIDUnionNode& node) {
        // TODO SERVER-75587 should implement this.
        return;
    }

    void operator()(const ABT& n, const BinaryJoinNode& node) {
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // We cannot satisfy limit-skip requirements.
            return;
        }
        if (getPropertyConst<DistributionRequirement>(_physProps)
                .getDistributionAndProjections()
                ._type != DistributionType::Centralized) {
            // For now we only support centralized distribution.
            return;
        }

        const GroupIdType leftGroupId =
            node.getLeftChild().cast<MemoLogicalDelegatorNode>()->getGroupId();
        const GroupIdType rightGroupId =
            node.getRightChild().cast<MemoLogicalDelegatorNode>()->getGroupId();

        const LogicalProps& leftLogicalProps = _memo.getLogicalProps(leftGroupId);
        const LogicalProps& rightLogicalProps = _memo.getLogicalProps(rightGroupId);

        const ProjectionNameSet& leftProjections =
            getPropertyConst<ProjectionAvailability>(leftLogicalProps).getProjections();
        const ProjectionNameSet& rightProjections =
            getPropertyConst<ProjectionAvailability>(rightLogicalProps).getProjections();

        PhysProps leftPhysProps = _physProps;
        PhysProps rightPhysProps = _physProps;

        {
            auto reqProjections =
                getPropertyConst<ProjectionRequirement>(_physProps).getProjections();

            // Add expression references to requirements.
            ProjectionNameSet references = collectVariableReferences(n);
            for (const auto& varName : references) {
                reqProjections.emplace_back(varName);
            }

            // Split required projections between inner and outer side.
            ProjectionNameOrderPreservingSet leftChildProjections;
            ProjectionNameOrderPreservingSet rightChildProjections;

            for (const ProjectionName& projectionName : reqProjections.getVector()) {

                if (leftProjections.count(projectionName) > 0) {
                    leftChildProjections.emplace_back(projectionName);
                } else if (rightProjections.count(projectionName) > 0) {
                    rightChildProjections.emplace_back(projectionName);
                } else {
                    uasserted(6624304,
                              "Required projection must appear in either the left or the right "
                              "child projections");
                    return;
                }
            }

            setPropertyOverwrite<ProjectionRequirement>(leftPhysProps,
                                                        std::move(leftChildProjections));
            setPropertyOverwrite<ProjectionRequirement>(rightPhysProps,
                                                        std::move(rightChildProjections));
        }

        if (hasProperty<CollationRequirement>(_physProps)) {
            const auto& collationSpec =
                getPropertyConst<CollationRequirement>(_physProps).getCollationSpec();

            // Split collation between inner and outer side.
            const CollationSplitResult& collationSplit = splitCollationSpec(
                boost::none /*ridProjName*/, collationSpec, leftProjections, rightProjections);
            if (!collationSplit._validSplit) {
                return;
            }

            setPropertyOverwrite<CollationRequirement>(leftPhysProps,
                                                       collationSplit._leftCollation);
            setPropertyOverwrite<CollationRequirement>(rightPhysProps,
                                                       collationSplit._leftCollation);
        }

        // TODO: consider hash join if the predicate is equality.
        ABT nlj = make<NestedLoopJoinNode>(std::move(node.getJoinType()),
                                           std::move(node.getCorrelatedProjectionNames()),
                                           std::move(node.getFilter()),
                                           std::move(node.getLeftChild()),
                                           std::move(node.getRightChild()));
        NestedLoopJoinNode& newNode = *nlj.cast<NestedLoopJoinNode>();

        optimizeChildren<NestedLoopJoinNode, PhysicalRewriteType::NLJ>(
            _queue,
            kDefaultPriority,
            std::move(nlj),
            ChildPropsType{{&newNode.getLeftChild(), std::move(leftPhysProps)},
                           {&newNode.getRightChild(), std::move(rightPhysProps)}});
    }

    void operator()(const ABT& /*n*/, const UnionNode& node) {
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // We cannot satisfy limit-skip requirements.
            return;
        }
        if (hasProperty<CollationRequirement>(_physProps)) {
            // In general we cannot satisfy collation requirements.
            // TODO: This may be possible with a merge sort type of node.
            return;
        }

        // Only need to propagate the required projection set.
        ABT physicalUnion = make<UnionNode>(
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections().getVector(),
            node.nodes());

        // Optimize each child under the same physical properties.
        ChildPropsType childProps;
        for (auto& child : physicalUnion.cast<UnionNode>()->nodes()) {
            PhysProps newProps = _physProps;
            childProps.emplace_back(&child, std::move(newProps));
        }

        optimizeChildren<UnionNode, PhysicalRewriteType::Union>(
            _queue, kDefaultPriority, std::move(physicalUnion), std::move(childProps));
    }

    void operator()(const ABT& n, const GroupByNode& node) {
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // We cannot satisfy limit-skip requirements.
            // TODO: consider an optimization where we keep track of at most "limit" groups.
            return;
        }
        if (hasProperty<CollationRequirement>(_physProps)) {
            // In general we cannot satisfy collation requirements.
            // TODO: consider stream group-by.
            return;
        }

        if (propertyAffectsProjections<DistributionRequirement>(
                _physProps, node.getAggregationProjectionNames())) {
            // We cannot satisfy distribution on the aggregations.
            return;
        }

        const ProjectionNameVector& groupByProjections = node.getGroupByProjectionNames();

        const bool isLocal = node.getType() == GroupNodeType::Local;
        if (!isLocal) {
            // We are constrained in terms of distribution only if we are a global or complete agg.

            const auto& distribAndProjections =
                getPropertyConst<DistributionRequirement>(_physProps)
                    .getDistributionAndProjections();
            switch (distribAndProjections._type) {
                case DistributionType::UnknownPartitioning:
                case DistributionType::RoundRobin:
                    // Cannot satisfy unknown or round-robin partitioning.
                    return;

                case DistributionType::HashPartitioning: {
                    ProjectionNameSet groupByProjectionSet;
                    for (const ProjectionName& projectionName : groupByProjections) {
                        groupByProjectionSet.insert(projectionName);
                    }
                    for (const ProjectionName& projectionName :
                         distribAndProjections._projectionNames) {
                        if (groupByProjectionSet.count(projectionName) == 0) {
                            // We can only be partitioned on projections on which we group.
                            return;
                        }
                    }
                    break;
                }

                case DistributionType::RangePartitioning:
                    if (distribAndProjections._projectionNames != groupByProjections) {
                        // For range partitioning we need to be partitioned exactly in the same
                        // order as our group-by projections.
                        return;
                    }
                    break;

                default:
                    break;
            }
        }

        PhysProps newProps = _physProps;

        // TODO: remove RepetitionEstimate if the subtree does not use bound variables.
        // TODO: this is not the case for stream group-by.

        // Specifically do not propagate limit-skip.
        removeProperty<LimitSkipRequirement>(newProps);

        getProperty<DistributionRequirement>(newProps).setDisableExchanges(isLocal);

        // Iterate over the aggregation expressions and only add those required.
        ABTVector aggregationProjections;
        ProjectionNameVector aggregationProjectionNames;
        ProjectionNameSet projectionsToAdd;
        for (const ProjectionName& groupByProjName : groupByProjections) {
            projectionsToAdd.insert(groupByProjName);
        }

        const auto& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();
        for (size_t aggIndex = 0; aggIndex < node.getAggregationExpressions().size(); aggIndex++) {
            const ProjectionName& aggProjectionName =
                node.getAggregationProjectionNames().at(aggIndex);

            if (requiredProjections.find(aggProjectionName)) {
                // We require this agg expression.
                aggregationProjectionNames.push_back(aggProjectionName);
                const ABT& aggExpr = node.getAggregationExpressions().at(aggIndex);
                aggregationProjections.push_back(aggExpr);

                // Add all references this expression requires.
                VariableEnvironment::walkVariables(
                    aggExpr, [&](const Variable& var) { projectionsToAdd.insert(var.name()); });
            }
        }

        addRemoveProjectionsToProperties(
            newProps, projectionsToAdd, node.getAggregationProjectionNames());

        ABT physicalGroupBy = make<GroupByNode>(groupByProjections,
                                                std::move(aggregationProjectionNames),
                                                std::move(aggregationProjections),
                                                node.getType(),
                                                node.getChild());
        optimizeChild<GroupByNode, PhysicalRewriteType::HashGroup>(
            _queue, kDefaultPriority, std::move(physicalGroupBy), std::move(newProps));
    }

    void operator()(const ABT& n, const UnwindNode& node) {
        const ProjectionName& pidProjectionName = node.getPIDProjectionName();
        const ProjectionNameVector& projectionNames = {(node.getProjectionName()),
                                                       pidProjectionName};

        if (propertyAffectsProjections<DistributionRequirement>(_physProps, projectionNames)) {
            // We cannot satisfy distribution on the unwound output, or pid.
            return;
        }
        if (propertyAffectsProjections<CollationRequirement>(_physProps, projectionNames)) {
            // We cannot satisfy collation on the output.
            return;
        }
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            // Cannot satisfy limit-skip.
            return;
        }

        PhysProps newProps = _physProps;
        addRemoveProjectionsToProperties(
            newProps, collectVariableReferences(n), ProjectionNameVector{pidProjectionName});

        // Specifically do not propagate limit-skip.
        removeProperty<LimitSkipRequirement>(newProps);
        // Keep collation property if given it does not affect output.

        getProperty<DistributionRequirement>(newProps).setDisableExchanges(false);

        ABT physicalUnwind = n;
        optimizeChild<UnwindNode, PhysicalRewriteType::Unwind>(
            _queue, kDefaultPriority, std::move(physicalUnwind), std::move(newProps));
    }

    void operator()(const ABT& /*n*/, const CollationNode& node) {
        if (getPropertyConst<DistributionRequirement>(_physProps)
                .getDistributionAndProjections()
                ._type != DistributionType::Centralized) {
            // We can only pick up collation under centralized (but we can enforce under any
            // distribution).
            return;
        }

        optimizeSimplePropertyNode<CollationNode,
                                   CollationRequirement,
                                   PhysicalRewriteType::Collation>(node);
    }

    void operator()(const ABT& /*n*/, const LimitSkipNode& node) {
        // We can pick-up limit-skip under any distribution (but enforce under centralized or
        // replicated).

        PhysProps newProps = _physProps;
        LimitSkipRequirement newProp = node.getProperty();

        removeProperty<LimitEstimate>(newProps);

        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            const LimitSkipRequirement& required =
                getPropertyConst<LimitSkipRequirement>(_physProps);
            LimitSkipRequirement merged(required.getLimit(), required.getSkip());

            combineLimitSkipProperties(merged, newProp);
            // Continue with new unenforced requirement.
            newProp = std::move(merged);
        }

        setPropertyOverwrite<LimitSkipRequirement>(newProps, std::move(newProp));
        getProperty<DistributionRequirement>(newProps).setDisableExchanges(false);

        optimizeUnderNewProperties<PhysicalRewriteType::LimitSkip>(
            _queue, kDefaultPriority, node.getChild(), std::move(newProps));
    }

    void operator()(const ABT& /*n*/, const ExchangeNode& node) {
        optimizeSimplePropertyNode<ExchangeNode,
                                   DistributionRequirement,
                                   PhysicalRewriteType::Exchange>(node);
    }

    void operator()(const ABT& n, const RootNode& node) {
        PhysProps newProps = _physProps;

        ABT rootNode = make<Blackhole>();
        if (hasProperty<ProjectionRequirement>(newProps)) {
            auto& projections = getProperty<ProjectionRequirement>(newProps).getProjections();
            for (const auto& projName : node.getProperty().getProjections().getVector()) {
                projections.emplace_back(projName);
            }
            rootNode = make<RootNode>(projections, n.cast<RootNode>()->getChild());
        } else {
            setPropertyOverwrite<ProjectionRequirement>(newProps, node.getProperty());
            rootNode = n;
        }

        getProperty<DistributionRequirement>(newProps).setDisableExchanges(false);

        optimizeChild<RootNode, PhysicalRewriteType::Root>(
            _queue, kDefaultPriority, std::move(rootNode), std::move(newProps));
    }

    template <typename T>
    void operator()(const ABT& /*n*/, const T& /*node*/) {
        static_assert(!canBeLogicalNode<T>(), "Logical node must implement its visitor.");
    }

    ImplementationVisitor(const Metadata& metadata,
                          const Memo& memo,
                          const QueryHints& hints,
                          const RIDProjectionsMap& ridProjections,
                          PrefixId& prefixId,
                          SpoolIdGenerator& spoolId,
                          PhysRewriteQueue& queue,
                          const PhysProps& physProps,
                          const LogicalProps& logicalProps,
                          const PathToIntervalFn& pathToInterval)
        : _metadata(metadata),
          _memo(memo),
          _hints(hints),
          _ridProjections(ridProjections),
          _prefixId(prefixId),
          _spoolId(spoolId),
          _queue(queue),
          _physProps(physProps),
          _logicalProps(logicalProps),
          _pathToInterval(pathToInterval) {}

private:
    template <class NodeType, class PropType, PhysicalRewriteType rule>
    void optimizeSimplePropertyNode(const NodeType& node) {
        const PropType& nodeProp = node.getProperty();
        PhysProps newProps = _physProps;
        setPropertyOverwrite<PropType>(newProps, nodeProp);

        getProperty<DistributionRequirement>(newProps).setDisableExchanges(false);
        optimizeUnderNewProperties<rule>(
            _queue, kDefaultPriority, node.getChild(), std::move(newProps));
    }

    struct IndexAvailableDirections {
        // For each equality prefix, keep track if we can match against forward or backward
        // direction.
        bool _forward = true;
        bool _backward = true;
    };

    boost::optional<std::vector<IndexAvailableDirections>> indexSatisfiesCollation(
        const IndexCollationSpec& indexCollationSpec,
        const CandidateIndexEntry& candidateIndexEntry,
        const ProjectionCollationSpec& requiredCollationSpec,
        const ProjectionName& ridProjName) {
        const auto& eqPrefixes = candidateIndexEntry._eqPrefixes;
        if (requiredCollationSpec.empty()) {
            // We are not constrained. Both forward and reverse available for each eq prefix.
            return {{eqPrefixes.size(), IndexAvailableDirections{}}};
        }

        size_t currentEqPrefix = 0;
        // Add result for first equality prefix.
        std::vector<IndexAvailableDirections> result(1);

        size_t collationSpecIndex = 0;
        bool indexSuitable = true;
        const auto& fieldProjections = candidateIndexEntry._fieldProjectionMap._fieldProjections;

        const auto updateDirectionsFn = [&result](const CollationOp availableOp,
                                                  const CollationOp reqOp) {
            result.back()._forward &= collationOpsCompatible(availableOp, reqOp);
            result.back()._backward &=
                collationOpsCompatible(reverseCollationOp(availableOp), reqOp);
        };

        // Verify the index is compatible with our collation requirement, and can deliver the right
        // order of paths. Note: we are iterating to index one past the size. We assume there is an
        // implicit rid index field which is collated in increasing order.
        for (size_t indexField = 0; indexField < indexCollationSpec.size() + 1; indexField++) {
            if (currentEqPrefix < eqPrefixes.size() - 1 &&
                indexField == eqPrefixes.at(currentEqPrefix + 1)._startPos) {
                currentEqPrefix++;
                result.push_back(IndexAvailableDirections{});
            }

            const auto& [reqProjName, reqOp] = requiredCollationSpec.at(collationSpecIndex);

            if (indexField < indexCollationSpec.size()) {
                const auto predType = candidateIndexEntry._predTypes.at(indexField);

                auto it = fieldProjections.find(FieldNameType{encodeIndexKeyName(indexField)});
                if (it == fieldProjections.cend()) {
                    // No bound projection for this index field.
                    if (predType != IndexFieldPredType::SimpleEquality) {
                        // We cannot satisfy the rest of the collation requirements.
                        indexSuitable = false;
                        break;
                    }
                    continue;
                }
                const ProjectionName& projName = it->second;

                switch (predType) {
                    case IndexFieldPredType::SimpleEquality:
                        // We do not need to collate this field because of equality.
                        if (reqProjName == projName) {
                            // We can satisfy the next collation requirement independent of
                            // collation op.
                            if (++collationSpecIndex >= requiredCollationSpec.size()) {
                                break;
                            }
                        }
                        continue;

                    case IndexFieldPredType::Compound:
                        // Cannot satisfy collation if we have a compound predicate on this field.
                        indexSuitable = false;
                        break;

                    case IndexFieldPredType::SimpleInequality:
                    case IndexFieldPredType::Unbound:
                        if (reqProjName != projName) {
                            indexSuitable = false;
                            break;
                        }
                        updateDirectionsFn(indexCollationSpec.at(indexField)._op, reqOp);
                        break;
                }
                if (!indexSuitable) {
                    break;
                }
            } else {
                // If we fall through here, we are trying to satisfy a trailing collation
                // requirement on rid.
                if (reqProjName != ridProjName ||
                    candidateIndexEntry._intervalPrefixSize != indexCollationSpec.size()) {
                    indexSuitable = false;
                    break;
                }
                updateDirectionsFn(CollationOp::Ascending, reqOp);
            }

            if (!result.back()._forward && !result.back()._backward) {
                indexSuitable = false;
                break;
            }
            if (++collationSpecIndex >= requiredCollationSpec.size()) {
                break;
            }
        }

        if (!indexSuitable || collationSpecIndex < requiredCollationSpec.size()) {
            return {};
        }

        // Pad result to match the number of prefixes. Allow forward and back by default.
        result.resize(eqPrefixes.size());
        return result;
    }

    /**
     * Check if we are under index-only requirements and expression introduces dependency on scan
     * projection.
     */
    bool checkIntroducesScanProjectionUnderIndexOnly(const ProjectionNameSet& references) {
        return hasProperty<IndexingAvailability>(_logicalProps) &&
            getPropertyConst<IndexingRequirement>(_physProps).getIndexReqTarget() ==
            IndexReqTarget::Index &&
            references.find(
                getPropertyConst<IndexingAvailability>(_logicalProps).getScanProjection()) !=
            references.cend();
    }

    bool distributionsCompatible(const IndexReqTarget target,
                                 const DistributionAndPaths& distributionAndPaths,
                                 const ProjectionName& scanProjection,
                                 const LogicalProps& scanLogicalProps,
                                 const PartialSchemaRequirements& reqMap,
                                 bool& canUseParallelScan) {
        const DistributionRequirement& required =
            getPropertyConst<DistributionRequirement>(_physProps);
        const auto& distribAndProjections = required.getDistributionAndProjections();

        const auto& scanDistributions =
            getPropertyConst<DistributionAvailability>(scanLogicalProps).getDistributionSet();

        switch (distribAndProjections._type) {
            case DistributionType::Centralized:
                return scanDistributions.count({DistributionType::Centralized}) > 0 ||
                    scanDistributions.count({DistributionType::Replicated}) > 0;

            case DistributionType::Replicated:
                return scanDistributions.count({DistributionType::Replicated}) > 0;

            case DistributionType::RoundRobin:
                if (target == IndexReqTarget::Seek) {
                    // We can satisfy Seek with RoundRobin if we can scan the collection in
                    // parallel.
                    return scanDistributions.count({DistributionType::UnknownPartitioning}) > 0;
                }

                // TODO: Are two round robin distributions compatible?
                return false;

            case DistributionType::UnknownPartitioning:
                if (target == IndexReqTarget::Index) {
                    // We cannot satisfy unknown partitioning with an index as (unlike parallel
                    // collection scan) we currently cannot perform a parallel index scan.
                    return false;
                }

                if (scanDistributions.count({DistributionType::UnknownPartitioning}) > 0) {
                    canUseParallelScan = true;
                    return true;
                }
                return false;

            case DistributionType::HashPartitioning:
            case DistributionType::RangePartitioning: {
                if (distribAndProjections._type != distributionAndPaths._type) {
                    return false;
                }

                size_t distributionPartitionIndex = 0;
                const ProjectionNameVector& requiredProjections =
                    distribAndProjections._projectionNames;

                for (const ABT& partitioningPath : distributionAndPaths._paths) {
                    if (auto proj = reqMap.findProjection(
                            PartialSchemaKey{scanProjection, partitioningPath});
                        proj && *proj == requiredProjections.at(distributionPartitionIndex)) {
                        distributionPartitionIndex++;
                    } else {
                        return false;
                    }
                }

                return distributionPartitionIndex == requiredProjections.size();
            }

            default:
                MONGO_UNREACHABLE;
        }
    }

    void setCollationForRIDIntersect(const CollationSplitResult& collationSplit,
                                     PhysProps& leftPhysProps,
                                     PhysProps& rightPhysProps) {
        if (collationSplit._leftCollation.empty()) {
            removeProperty<CollationRequirement>(leftPhysProps);
        } else {
            setPropertyOverwrite<CollationRequirement>(leftPhysProps,
                                                       collationSplit._leftCollation);
        }

        if (collationSplit._rightCollation.empty()) {
            removeProperty<CollationRequirement>(rightPhysProps);
        } else {
            setPropertyOverwrite<CollationRequirement>(rightPhysProps,
                                                       collationSplit._rightCollation);
        }
    }

    void optimizeRIDIntersect(const bool isIndex,
                              const bool dedupRID,
                              const bool useMergeJoin,
                              const ProjectionName& ridProjectionName,
                              const CollationSplitResult& collationLeftRightSplit,
                              const CollationSplitResult& collationRightLeftSplit,
                              const CEType intersectedCE,
                              const CEType leftCE,
                              const CEType rightCE,
                              const PhysProps& leftPhysProps,
                              const PhysProps& rightPhysProps,
                              const ABT& leftChild,
                              const ABT& rightChild) {
        if (isIndex && collationRightLeftSplit._validSplit &&
            (!collationLeftRightSplit._validSplit || leftCE > rightCE)) {
            // Need to reverse the left and right side as the left collation split is not valid, or
            // to use the larger CE as the other side.
            optimizeRIDIntersect(true /*isIndex*/,
                                 dedupRID,
                                 useMergeJoin,
                                 ridProjectionName,
                                 collationRightLeftSplit,
                                 {},
                                 intersectedCE,
                                 rightCE,
                                 leftCE,
                                 rightPhysProps,
                                 leftPhysProps,
                                 rightChild,
                                 leftChild);
            return;
        }
        if (!collationLeftRightSplit._validSplit) {
            return;
        }

        if (isIndex) {
            if (useMergeJoin && !_hints._disableMergeJoinRIDIntersect) {
                // Try a merge join on RID since both of our children only have equality
                // predicates.

                PhysProps leftPhysPropsLocal = leftPhysProps;
                PhysProps rightPhysPropsLocal = rightPhysProps;

                // Add collation requirement on rid to both sides if needed.
                CollationSplitResult split = collationLeftRightSplit;
                if (split._leftCollation.empty() ||
                    split._leftCollation.back().first != ridProjectionName) {
                    split._leftCollation.emplace_back(ridProjectionName, CollationOp::Ascending);
                }
                if (split._rightCollation.empty() ||
                    split._rightCollation.back().first != ridProjectionName) {
                    split._rightCollation.emplace_back(ridProjectionName, CollationOp::Ascending);
                }
                setCollationForRIDIntersect(split, leftPhysPropsLocal, rightPhysPropsLocal);

                if (dedupRID) {
                    getProperty<IndexingRequirement>(leftPhysPropsLocal)
                        .setDedupRID(true /*dedupRID*/);
                    getProperty<IndexingRequirement>(rightPhysPropsLocal)
                        .setDedupRID(true /*dedupRID*/);
                }

                ChildPropsType childProps;
                auto builder = lowerRIDIntersectMergeJoin(_prefixId,
                                                          ridProjectionName,
                                                          intersectedCE,
                                                          leftCE,
                                                          rightCE,
                                                          leftPhysPropsLocal,
                                                          rightPhysPropsLocal,
                                                          {leftChild},
                                                          {rightChild},
                                                          childProps);
                optimizeChildrenNoAssert(_queue,
                                         kDefaultPriority,
                                         PhysicalRewriteType::RIDIntersectMergeJoin,
                                         std::move(builder._node),
                                         std::move(childProps),
                                         std::move(builder._nodeCEMap));
            } else {
                if (!_hints._disableHashJoinRIDIntersect) {
                    // Try a HashJoin. Propagate dedupRID on left and right indexing
                    // requirements.

                    PhysProps leftPhysPropsLocal = leftPhysProps;
                    PhysProps rightPhysPropsLocal = rightPhysProps;

                    setCollationForRIDIntersect(
                        collationLeftRightSplit, leftPhysPropsLocal, rightPhysPropsLocal);
                    if (dedupRID) {
                        getProperty<IndexingRequirement>(leftPhysPropsLocal)
                            .setDedupRID(true /*dedupRID*/);
                        getProperty<IndexingRequirement>(rightPhysPropsLocal)
                            .setDedupRID(true /*dedupRID*/);
                    }

                    ChildPropsType childProps;
                    auto builder = lowerRIDIntersectHashJoin(_prefixId,
                                                             ridProjectionName,
                                                             intersectedCE,
                                                             leftCE,
                                                             rightCE,
                                                             leftPhysPropsLocal,
                                                             rightPhysPropsLocal,
                                                             {leftChild},
                                                             {rightChild},
                                                             childProps);
                    optimizeChildrenNoAssert(_queue,
                                             kDefaultPriority,
                                             PhysicalRewriteType::RIDIntersectHashJoin,
                                             std::move(builder._node),
                                             std::move(childProps),
                                             std::move(builder._nodeCEMap));
                }

                // We can only attempt this strategy if we have no collation requirements.
                if (!_hints._disableGroupByAndUnionRIDIntersect && dedupRID &&
                    collationLeftRightSplit._leftCollation.empty() &&
                    collationLeftRightSplit._rightCollation.empty()) {
                    // Try a Union+GroupBy. left and right indexing requirements are already
                    // initialized to not dedup.

                    PhysProps leftPhysPropsLocal = leftPhysProps;
                    PhysProps rightPhysPropsLocal = rightPhysProps;

                    setCollationForRIDIntersect(
                        collationLeftRightSplit, leftPhysPropsLocal, rightPhysPropsLocal);

                    ChildPropsType childProps;
                    auto builder = lowerRIDIntersectGroupBy(_prefixId,
                                                            ridProjectionName,
                                                            intersectedCE,
                                                            leftCE,
                                                            rightCE,
                                                            _physProps,
                                                            leftPhysPropsLocal,
                                                            rightPhysPropsLocal,
                                                            {leftChild},
                                                            {rightChild},
                                                            childProps);
                    optimizeChildrenNoAssert(_queue,
                                             kDefaultPriority,
                                             PhysicalRewriteType::RIDIntersectGroupBy,
                                             std::move(builder._node),
                                             std::move(childProps),
                                             std::move(builder._nodeCEMap));
                }
            }
        } else {
            ABT nlj = make<NestedLoopJoinNode>(JoinType::Inner,
                                               ProjectionNameSet{ridProjectionName},
                                               Constant::boolean(true),
                                               leftChild,
                                               rightChild);

            PhysProps leftPhysPropsLocal = leftPhysProps;
            PhysProps rightPhysPropsLocal = rightPhysProps;
            setCollationForRIDIntersect(
                collationLeftRightSplit, leftPhysPropsLocal, rightPhysPropsLocal);

            optimizeChildren<NestedLoopJoinNode, PhysicalRewriteType::IndexFetch>(
                _queue,
                kDefaultPriority,
                std::move(nlj),
                std::move(leftPhysPropsLocal),
                std::move(rightPhysPropsLocal));
        }
    }

    // We don't own any of those:
    const Metadata& _metadata;
    const Memo& _memo;
    const QueryHints& _hints;
    const RIDProjectionsMap& _ridProjections;
    PrefixId& _prefixId;
    SpoolIdGenerator& _spoolId;
    PhysRewriteQueue& _queue;
    const PhysProps& _physProps;
    const LogicalProps& _logicalProps;
    const PathToIntervalFn& _pathToInterval;
};

void addImplementers(const Metadata& metadata,
                     const Memo& memo,
                     const QueryHints& hints,
                     const RIDProjectionsMap& ridProjections,
                     PrefixId& prefixId,
                     SpoolIdGenerator& spoolId,
                     const PhysProps& physProps,
                     PhysQueueAndImplPos& queue,
                     const LogicalProps& logicalProps,
                     const OrderPreservingABTSet& logicalNodes,
                     const PathToIntervalFn& pathToInterval) {
    ImplementationVisitor visitor(metadata,
                                  memo,
                                  hints,
                                  ridProjections,
                                  prefixId,
                                  spoolId,
                                  queue._queue,
                                  physProps,
                                  logicalProps,
                                  pathToInterval);
    while (queue._lastImplementedNodePos < logicalNodes.size()) {
        logicalNodes.at(queue._lastImplementedNodePos++).visit(visitor);
    }
}

}  // namespace mongo::optimizer::cascades
