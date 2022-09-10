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
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"

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

        const auto& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();
        const ProjectionName& ridProjName = _ridProjections.at(node.getScanDefName());
        const bool needsRID = requiredProjections.find(ridProjName).second;

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
                _memo.getMetadata()._scanDefs.at(node.getScanDefName()).getDistributionAndPaths(),
                node.getProjectionName(),
                _logicalProps,
                {},
                canUseParallelScan)) {
            return;
        }

        FieldProjectionMap fieldProjectionMap;
        for (const ProjectionName& required : requiredProjections.getVector()) {
            if (required == node.getProjectionName()) {
                fieldProjectionMap._rootProjection = node.getProjectionName();
            } else {
                // Regular scan node can satisfy only using its root projection (not fields).
                return;
            }
        }

        if (indexReqTarget == IndexReqTarget::Seek) {
            NodeCEMap nodeCEMap;

            ABT physicalSeek =
                make<SeekNode>(ridProjName, std::move(fieldProjectionMap), node.getScanDefName());
            // If optimizing a Seek, override CE to 1.0.
            nodeCEMap.emplace(physicalSeek.cast<Node>(), 1.0);

            ABT limitSkip =
                make<LimitSkipNode>(LimitSkipRequirement{1, 0}, std::move(physicalSeek));
            nodeCEMap.emplace(limitSkip.cast<Node>(), 1.0);

            optimizeChildrenNoAssert(_queue,
                                     kDefaultPriority,
                                     PhysicalRewriteType::Seek,
                                     std::move(limitSkip),
                                     {},
                                     std::move(nodeCEMap));
        } else {
            if (needsRID) {
                fieldProjectionMap._ridProjection = ridProjName;
            }
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

        ProjectionName ridProjName;
        bool needsRID = false;
        if (hasProperty<IndexingAvailability>(_logicalProps)) {
            ridProjName = _ridProjections.at(
                getPropertyConst<IndexingAvailability>(_logicalProps).getScanDefName());
            needsRID = requiredProjections.find(ridProjName).second;
        }
        if (needsRID && !node.getHasRID()) {
            // We cannot provide RID.
            return;
        }

        NodeCEMap nodeCEMap;
        ABT physNode = make<CoScanNode>();
        if (node.getArraySize() == 0) {
            nodeCEMap.emplace(physNode.cast<Node>(), 0.0);

            physNode =
                make<LimitSkipNode>(properties::LimitSkipRequirement{0, 0}, std::move(physNode));
            nodeCEMap.emplace(physNode.cast<Node>(), 0.0);

            for (const ProjectionName& boundProjName : node.binder().names()) {
                if (requiredProjections.find(boundProjName).second) {
                    physNode = make<EvaluationNode>(
                        boundProjName, Constant::nothing(), std::move(physNode));
                    nodeCEMap.emplace(physNode.cast<Node>(), 0.0);
                }
            }
            if (needsRID) {
                physNode = make<EvaluationNode>(
                    std::move(ridProjName), Constant::nothing(), std::move(physNode));
                nodeCEMap.emplace(physNode.cast<Node>(), 0.0);
            }
        } else {
            nodeCEMap.emplace(physNode.cast<Node>(), 1.0);

            physNode =
                make<LimitSkipNode>(properties::LimitSkipRequirement{1, 0}, std::move(physNode));
            nodeCEMap.emplace(physNode.cast<Node>(), 1.0);

            const ProjectionName valueScanProj = _prefixId.getNextId("valueScan");
            physNode =
                make<EvaluationNode>(valueScanProj, node.getValueArray(), std::move(physNode));
            nodeCEMap.emplace(physNode.cast<Node>(), 1.0);

            // Unwind the combined array constant and pick an element for each required projection
            // in sequence.
            physNode = make<UnwindNode>(valueScanProj,
                                        _prefixId.getNextId("valueScanPid"),
                                        false /*retainNonArrays*/,
                                        std::move(physNode));
            nodeCEMap.emplace(physNode.cast<Node>(), 1.0);

            const auto getElementFn = [&valueScanProj](const size_t index) {
                return make<FunctionCall>(
                    "getElement", makeSeq(make<Variable>(valueScanProj), Constant::int32(index)));
            };

            // Iterate over the bound projections here as opposed to the required projections, since
            // the array elements are ordered accordingly. Skip over the first element (this is the
            // row id).
            const ProjectionNameVector& boundProjNames = node.binder().names();
            for (size_t i = 0; i < boundProjNames.size(); i++) {
                const ProjectionName& boundProjName = boundProjNames.at(i);
                if (requiredProjections.find(boundProjName).second) {
                    physNode = make<EvaluationNode>(boundProjName,
                                                    getElementFn(i + (node.getHasRID() ? 1 : 0)),
                                                    std::move(physNode));
                    nodeCEMap.emplace(physNode.cast<Node>(), node.getArraySize());
                }
            }

            if (needsRID) {
                // Obtain row id from first element of the array.
                physNode = make<EvaluationNode>(
                    std::move(ridProjName), getElementFn(0), std::move(physNode));
                nodeCEMap.emplace(physNode.cast<Node>(), node.getArraySize());
            }
        }

        optimizeChildrenNoAssert(_queue,
                                 kDefaultPriority,
                                 PhysicalRewriteType::ValueScan,
                                 std::move(physNode),
                                 {},
                                 std::move(nodeCEMap));
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

        VariableNameSetType references = collectVariableReferences(n);
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

        VariableNameSetType references = collectVariableReferences(n);
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
        const auto& scanDef = _memo.getMetadata()._scanDefs.at(scanDefName);


        // We do not check indexDefs to be empty here. We want to allow evaluations to be covered
        // via a physical scan even in the absence of indexes.

        const IndexingRequirement& requirements = getPropertyConst<IndexingRequirement>(_physProps);
        const CandidateIndexes& candidateIndexes = node.getCandidateIndexes();
        const IndexReqTarget indexReqTarget = requirements.getIndexReqTarget();
        switch (indexReqTarget) {
            case IndexReqTarget::Complete:
                if (_hints._disableScan) {
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

        const auto& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();
        const ProjectionName& ridProjName = _ridProjections.at(scanDefName);
        const bool needsRID = requiredProjections.find(ridProjName).second;

        const ProjectionName& scanProjectionName = indexingAvailability.getScanProjection();
        const GroupIdType scanGroupId = indexingAvailability.getScanGroupId();
        const LogicalProps& scanLogicalProps = _memo.getGroup(scanGroupId)._logicalProperties;
        const CEType scanGroupCE =
            getPropertyConst<CardinalityEstimate>(scanLogicalProps).getEstimate();

        const PartialSchemaRequirements& reqMap = node.getReqMap();

        if (indexReqTarget != IndexReqTarget::Index &&
            hasProperty<CollationRequirement>(_physProps)) {
            // PhysicalScan or Seek cannot satisfy any collation requirement.
            // TODO: consider rid?
            return;
        }

        for (const auto& [key, req] : reqMap) {
            if (key._projectionName != scanProjectionName) {
                // We can only satisfy partial schema requirements using our root projection.
                return;
            }
        }

        bool requiresRootProjection = false;
        {
            auto projectionsLeftToSatisfy = requiredProjections;
            if (needsRID) {
                projectionsLeftToSatisfy.erase(ridProjName);
            }
            if (indexReqTarget != IndexReqTarget::Index) {
                // Deliver root projection if required.
                requiresRootProjection = projectionsLeftToSatisfy.erase(scanProjectionName);
            }

            for (const auto& entry : reqMap) {
                if (entry.second.hasBoundProjectionName()) {
                    // Project field only if it required.
                    const ProjectionName& projectionName = entry.second.getBoundProjectionName();
                    projectionsLeftToSatisfy.erase(projectionName);
                }
            }
            if (!projectionsLeftToSatisfy.getVector().empty()) {
                // Unknown projections remain. Reject.
                return;
            }
        }

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
                    _memo.getGroup(requirements.getSatisfiedPartialIndexesGroupId())
                        ._logicalProperties)
                    .getSatisfiedPartialIndexes();

            // Consider all candidate indexes, and check if they satisfy the collation and
            // distribution requirements.
            for (const auto& candidateIndexEntry : node.getCandidateIndexes()) {
                const auto& indexDefName = candidateIndexEntry._indexDefName;
                const auto& indexDef = scanDef.getIndexDefs().at(indexDefName);

                if (!indexDef.getPartialReqMap().empty() &&
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
                if (!availableDirections._forward && !availableDirections._backward) {
                    // Failed to satisfy collation.
                    continue;
                }

                uassert(6624103,
                        "Either forward or backward direction must be available.",
                        availableDirections._forward || availableDirections._backward);

                auto indexProjectionMap = candidateIndexEntry._fieldProjectionMap;
                auto residualReqs = candidateIndexEntry._residualRequirements;
                removeRedundantResidualPredicates(
                    requiredProjections, residualReqs, indexProjectionMap);

                CEType indexCE = currentGroupCE;
                ResidualRequirementsWithCE residualReqsWithCE;
                std::vector<SelectivityType> indexPredSels;
                if (!residualReqs.empty()) {
                    PartialSchemaKeySet residualQueryKeySet;
                    for (const auto& [residualKey, residualReq, entryIndex] : residualReqs) {
                        auto entryIt = reqMap.cbegin();
                        std::advance(entryIt, entryIndex);
                        residualQueryKeySet.emplace(entryIt->first);
                        residualReqsWithCE.emplace_back(
                            residualKey, residualReq, partialSchemaKeyCE.at(entryIndex).second);
                    }

                    if (scanGroupCE > 0.0) {
                        size_t entryIndex = 0;
                        for (const auto& [key, req] : reqMap) {
                            if (residualQueryKeySet.count(key) == 0) {
                                indexPredSels.push_back(partialSchemaKeyCE.at(entryIndex).second /
                                                        scanGroupCE);
                            }
                            entryIndex++;
                        }

                        if (!indexPredSels.empty()) {
                            indexCE = scanGroupCE * ce::conjExponentialBackoff(indexPredSels);
                        }
                    }
                }

                const auto& intervals = candidateIndexEntry._intervals;
                ABT physNode = make<Blackhole>();
                NodeCEMap nodeCEMap;

                // TODO: consider pre-computing as part of the candidateIndexes structure.
                const auto singularInterval = CompoundIntervalReqExpr::getSingularDNF(intervals);
                const bool needsUniqueStage =
                    (!singularInterval || !areCompoundIntervalsEqualities(*singularInterval)) &&
                    indexDef.isMultiKey() && requirements.getDedupRID();

                indexProjectionMap._ridProjection =
                    (needsRID || needsUniqueStage) ? ridProjName : "";
                if (singularInterval) {
                    physNode =
                        make<IndexScanNode>(std::move(indexProjectionMap),
                                            IndexSpecification{scanDefName,
                                                               indexDefName,
                                                               *singularInterval,
                                                               !availableDirections._forward});
                    nodeCEMap.emplace(physNode.cast<Node>(), indexCE);
                } else {
                    physNode = lowerIntervals(_prefixId,
                                              ridProjName,
                                              std::move(indexProjectionMap),
                                              scanDefName,
                                              indexDefName,
                                              intervals,
                                              !availableDirections._forward,
                                              indexCE,
                                              scanGroupCE,
                                              nodeCEMap);
                }

                lowerPartialSchemaRequirements(scanGroupCE,
                                               std::move(indexPredSels),
                                               residualReqsWithCE,
                                               physNode,
                                               _pathToInterval,
                                               nodeCEMap);

                if (needsUniqueStage) {
                    // Insert unique stage if we need to, after the residual requirements.
                    physNode =
                        make<UniqueNode>(ProjectionNameVector{ridProjName}, std::move(physNode));
                    nodeCEMap.emplace(physNode.cast<Node>(), currentGroupCE);
                }

                optimizeChildrenNoAssert(_queue,
                                         kDefaultPriority,
                                         PhysicalRewriteType::SargableToIndex,
                                         std::move(physNode),
                                         {},
                                         std::move(nodeCEMap));
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
            ResidualRequirements residualReqs = scanParams->_residualRequirements;
            removeRedundantResidualPredicates(
                requiredProjections, residualReqs, fieldProjectionMap);

            if (indexReqTarget == IndexReqTarget::Complete && needsRID) {
                fieldProjectionMap._ridProjection = ridProjName;
            }
            if (requiresRootProjection) {
                fieldProjectionMap._rootProjection = scanProjectionName;
            }

            NodeCEMap nodeCEMap;
            ABT physNode = make<Blackhole>();
            CEType baseCE = 0.0;

            PhysicalRewriteType rule = PhysicalRewriteType::Uninitialized;
            if (indexReqTarget == IndexReqTarget::Complete) {
                baseCE = scanGroupCE;

                // Return a physical scan with field map.
                physNode = make<PhysicalScanNode>(
                    std::move(fieldProjectionMap), scanDefName, canUseParallelScan);
                nodeCEMap.emplace(physNode.cast<Node>(), baseCE);
                rule = PhysicalRewriteType::SargableToPhysicalScan;
            } else {
                baseCE = 1.0;

                // Try Seek with Limit 1.
                physNode = make<SeekNode>(ridProjName, std::move(fieldProjectionMap), scanDefName);
                nodeCEMap.emplace(physNode.cast<Node>(), baseCE);

                physNode = make<LimitSkipNode>(LimitSkipRequirement{1, 0}, std::move(physNode));
                nodeCEMap.emplace(physNode.cast<Node>(), baseCE);
                rule = PhysicalRewriteType::SargableToSeek;
            }

            ResidualRequirementsWithCE residualReqsWithCE;
            for (const auto& [residualKey, residualReq, entryIndex] : residualReqs) {
                residualReqsWithCE.emplace_back(
                    residualKey, residualReq, partialSchemaKeyCE.at(entryIndex).second);
            }

            lowerPartialSchemaRequirements(baseCE,
                                           {} /*indexPredSels*/,
                                           residualReqsWithCE,
                                           physNode,
                                           _pathToInterval,
                                           nodeCEMap);
            optimizeChildrenNoAssert(
                _queue, kDefaultPriority, rule, std::move(physNode), {}, std::move(nodeCEMap));
        }
    }

    void operator()(const ABT& /*n*/, const RIDIntersectNode& node) {
        const auto& indexingAvailability = getPropertyConst<IndexingAvailability>(_logicalProps);
        const std::string& scanDefName = indexingAvailability.getScanDefName();
        {
            const auto& scanDef = _memo.getMetadata()._scanDefs.at(scanDefName);
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
        if (isIndex && (!node.hasLeftIntervals() || !node.hasRightIntervals())) {
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

        const GroupIdType leftGroupId =
            node.getLeftChild().cast<MemoLogicalDelegatorNode>()->getGroupId();
        const GroupIdType rightGroupId =
            node.getRightChild().cast<MemoLogicalDelegatorNode>()->getGroupId();

        const LogicalProps& leftLogicalProps = _memo.getGroup(leftGroupId)._logicalProperties;
        const LogicalProps& rightLogicalProps = _memo.getGroup(rightGroupId)._logicalProperties;

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
            CEType estimatedRepetitions = hasProperty<RepetitionEstimate>(_physProps)
                ? getPropertyConst<RepetitionEstimate>(_physProps).getEstimate()
                : 1.0;
            estimatedRepetitions *=
                getPropertyConst<CardinalityEstimate>(leftLogicalProps).getEstimate();
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

        optimizeFn();

        if (isIndex) {
            switch (distrAndProjections._type) {
                case DistributionType::HashPartitioning:
                case DistributionType::RangePartitioning: {
                    // Specifically for index intersection, try propagating the requirement on one
                    // side and replicating the other.

                    const auto& leftDistributions =
                        getPropertyConst<DistributionAvailability>(leftLogicalProps)
                            .getDistributionSet();
                    const auto& rightDistributions =
                        getPropertyConst<DistributionAvailability>(rightLogicalProps)
                            .getDistributionSet();

                    if (leftDistributions.count(distrAndProjections) > 0) {
                        setPropertyOverwrite<DistributionRequirement>(leftPhysProps,
                                                                      distribRequirement);
                        setPropertyOverwrite<DistributionRequirement>(
                            rightPhysProps, DistributionRequirement{DistributionType::Replicated});
                        optimizeFn();
                    }

                    if (rightDistributions.count(distrAndProjections) > 0) {
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

        const LogicalProps& leftLogicalProps = _memo.getGroup(leftGroupId)._logicalProperties;
        const LogicalProps& rightLogicalProps = _memo.getGroup(rightGroupId)._logicalProperties;

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
            VariableNameSetType references = collectVariableReferences(n);
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
                "" /*ridProjName*/, collationSpec, leftProjections, rightProjections);
            if (!collationSplit._validSplit) {
                return;
            }

            setPropertyOverwrite<CollationRequirement>(leftPhysProps,
                                                       collationSplit._leftCollation);
            setPropertyOverwrite<CollationRequirement>(rightPhysProps,
                                                       collationSplit._leftCollation);
        }

        // TODO: consider hash join if the predicate is equality.
        ABT physicalJoin = n;
        BinaryJoinNode& newNode = *physicalJoin.cast<BinaryJoinNode>();

        optimizeChildren<BinaryJoinNode, PhysicalRewriteType::NLJ>(
            _queue,
            kDefaultPriority,
            std::move(physicalJoin),
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
        VariableNameSetType projectionsToAdd;
        for (const ProjectionName& groupByProjName : groupByProjections) {
            projectionsToAdd.insert(groupByProjName);
        }

        const auto& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();
        for (size_t aggIndex = 0; aggIndex < node.getAggregationExpressions().size(); aggIndex++) {
            const ProjectionName& aggProjectionName =
                node.getAggregationProjectionNames().at(aggIndex);

            if (requiredProjections.find(aggProjectionName).second) {
                // We require this agg expression.
                aggregationProjectionNames.push_back(aggProjectionName);
                const ABT& aggExpr = node.getAggregationExpressions().at(aggIndex);
                aggregationProjections.push_back(aggExpr);

                for (const Variable& var : VariableEnvironment::getVariables(aggExpr)._variables) {
                    // Add all references this expression requires.
                    projectionsToAdd.insert(var.name());
                }
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

    ImplementationVisitor(const Memo& memo,
                          const QueryHints& hints,
                          const RIDProjectionsMap& ridProjections,
                          PrefixId& prefixId,
                          PhysRewriteQueue& queue,
                          const PhysProps& physProps,
                          const LogicalProps& logicalProps,
                          const PathToIntervalFn& pathToInterval)
        : _memo(memo),
          _hints(hints),
          _ridProjections(ridProjections),
          _prefixId(prefixId),
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
        // Keep track if we can match against forward or backward direction.
        bool _forward = true;
        bool _backward = true;
    };

    IndexAvailableDirections indexSatisfiesCollation(
        const IndexCollationSpec& indexCollationSpec,
        const CandidateIndexEntry& candidateIndexEntry,
        const ProjectionCollationSpec& requiredCollationSpec,
        const ProjectionName& ridProjName) {
        if (requiredCollationSpec.empty()) {
            return {true, true};
        }

        IndexAvailableDirections result;
        size_t collationSpecIndex = 0;
        bool indexSuitable = true;
        const auto& fieldProjections = candidateIndexEntry._fieldProjectionMap._fieldProjections;

        const auto updateDirectionsFn = [&result](const CollationOp availableOp,
                                                  const CollationOp reqOp) {
            result._forward &= collationOpsCompatible(availableOp, reqOp);
            result._backward &= collationOpsCompatible(reverseCollationOp(availableOp), reqOp);
        };

        // Verify the index is compatible with our collation requirement, and can deliver the right
        // order of paths. Note: we are iterating to index one past the size. We assume there is an
        // implicit rid index field which is collated in increasing order.
        for (size_t indexField = 0; indexField < indexCollationSpec.size() + 1; indexField++) {
            const auto& [reqProjName, reqOp] = requiredCollationSpec.at(collationSpecIndex);

            if (indexField < indexCollationSpec.size()) {
                const bool needsCollation =
                    candidateIndexEntry._fieldsToCollate.count(indexField) > 0;

                auto it = fieldProjections.find(encodeIndexKeyName(indexField));
                if (it == fieldProjections.cend()) {
                    // No bound projection for this index field.
                    if (needsCollation) {
                        // We cannot satisfy the rest of the collation requirements.
                        indexSuitable = false;
                        break;
                    }
                    continue;
                }
                const ProjectionName& projName = it->second;

                if (!needsCollation) {
                    // We do not need to collate this field because of equality.
                    if (requiredCollationSpec.at(collationSpecIndex).first == projName) {
                        // We can satisfy the next collation requirement independent of collation
                        // op.
                        if (++collationSpecIndex >= requiredCollationSpec.size()) {
                            break;
                        }
                    }
                    continue;
                }

                if (reqProjName != projName) {
                    indexSuitable = false;
                    break;
                }
                updateDirectionsFn(indexCollationSpec.at(indexField)._op, reqOp);
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

            if (!result._forward && !result._backward) {
                indexSuitable = false;
                break;
            }
            if (++collationSpecIndex >= requiredCollationSpec.size()) {
                break;
            }
        }

        if (!indexSuitable || collationSpecIndex < requiredCollationSpec.size()) {
            return {false, false};
        }
        return result;
    }

    /**
     * Check if we are under index-only requirements and expression introduces dependency on scan
     * projection.
     */
    bool checkIntroducesScanProjectionUnderIndexOnly(const VariableNameSetType& references) {
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
                    auto it = reqMap.find(PartialSchemaKey{scanProjection, partitioningPath});
                    if (it == reqMap.cend()) {
                        return false;
                    }

                    if (it->second.getBoundProjectionName() !=
                        requiredProjections.at(distributionPartitionIndex)) {
                        return false;
                    }
                    distributionPartitionIndex++;
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

                NodeCEMap nodeCEMap;
                ChildPropsType childProps;
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

                ABT physNode = lowerRIDIntersectMergeJoin(_prefixId,
                                                          ridProjectionName,
                                                          intersectedCE,
                                                          leftCE,
                                                          rightCE,
                                                          leftPhysPropsLocal,
                                                          rightPhysPropsLocal,
                                                          leftChild,
                                                          rightChild,
                                                          nodeCEMap,
                                                          childProps);
                optimizeChildrenNoAssert(_queue,
                                         kDefaultPriority,
                                         PhysicalRewriteType::RIDIntersectMergeJoin,
                                         std::move(physNode),
                                         std::move(childProps),
                                         std::move(nodeCEMap));
            } else {
                if (!_hints._disableHashJoinRIDIntersect) {
                    // Try a HashJoin. Propagate dedupRID on left and right indexing
                    // requirements.

                    NodeCEMap nodeCEMap;
                    ChildPropsType childProps;
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

                    ABT physNode = lowerRIDIntersectHashJoin(_prefixId,
                                                             ridProjectionName,
                                                             intersectedCE,
                                                             leftCE,
                                                             rightCE,
                                                             leftPhysPropsLocal,
                                                             rightPhysPropsLocal,
                                                             leftChild,
                                                             rightChild,
                                                             nodeCEMap,
                                                             childProps);
                    optimizeChildrenNoAssert(_queue,
                                             kDefaultPriority,
                                             PhysicalRewriteType::RIDIntersectHashJoin,
                                             std::move(physNode),
                                             std::move(childProps),
                                             std::move(nodeCEMap));
                }

                // We can only attempt this strategy if we have no collation requirements.
                if (!_hints._disableGroupByAndUnionRIDIntersect && dedupRID &&
                    collationLeftRightSplit._leftCollation.empty() &&
                    collationLeftRightSplit._rightCollation.empty()) {
                    // Try a Union+GroupBy. left and right indexing requirements are already
                    // initialized to not dedup.

                    NodeCEMap nodeCEMap;
                    ChildPropsType childProps;
                    PhysProps leftPhysPropsLocal = leftPhysProps;
                    PhysProps rightPhysPropsLocal = rightPhysProps;

                    setCollationForRIDIntersect(
                        collationLeftRightSplit, leftPhysPropsLocal, rightPhysPropsLocal);

                    ABT physNode = lowerRIDIntersectGroupBy(_prefixId,
                                                            ridProjectionName,
                                                            intersectedCE,
                                                            leftCE,
                                                            rightCE,
                                                            _physProps,
                                                            leftPhysPropsLocal,
                                                            rightPhysPropsLocal,
                                                            leftChild,
                                                            rightChild,
                                                            nodeCEMap,
                                                            childProps);
                    optimizeChildrenNoAssert(_queue,
                                             kDefaultPriority,
                                             PhysicalRewriteType::RIDIntersectGroupBy,
                                             std::move(physNode),
                                             std::move(childProps),
                                             std::move(nodeCEMap));
                }
            }
        } else {
            ABT physicalJoin = make<BinaryJoinNode>(JoinType::Inner,
                                                    ProjectionNameSet{ridProjectionName},
                                                    Constant::boolean(true),
                                                    leftChild,
                                                    rightChild);

            PhysProps leftPhysPropsLocal = leftPhysProps;
            PhysProps rightPhysPropsLocal = rightPhysProps;
            setCollationForRIDIntersect(
                collationLeftRightSplit, leftPhysPropsLocal, rightPhysPropsLocal);

            optimizeChildren<BinaryJoinNode, PhysicalRewriteType::RIDIntersectNLJ>(
                _queue,
                kDefaultPriority,
                std::move(physicalJoin),
                std::move(leftPhysPropsLocal),
                std::move(rightPhysPropsLocal));
        }
    }

    // We don't own any of those:
    const Memo& _memo;
    const QueryHints& _hints;
    const RIDProjectionsMap& _ridProjections;
    PrefixId& _prefixId;
    PhysRewriteQueue& _queue;
    const PhysProps& _physProps;
    const LogicalProps& _logicalProps;
    const PathToIntervalFn& _pathToInterval;
};

void addImplementers(const Memo& memo,
                     const QueryHints& hints,
                     const RIDProjectionsMap& ridProjections,
                     PrefixId& prefixId,
                     PhysOptimizationResult& bestResult,
                     const properties::LogicalProps& logicalProps,
                     const OrderPreservingABTSet& logicalNodes,
                     const PathToIntervalFn& pathToInterval) {
    ImplementationVisitor visitor(memo,
                                  hints,
                                  ridProjections,
                                  prefixId,
                                  bestResult._queue,
                                  bestResult._physProps,
                                  logicalProps,
                                  pathToInterval);
    while (bestResult._lastImplementedNodePos < logicalNodes.size()) {
        logicalNodes.at(bestResult._lastImplementedNodePos++).visit(visitor);
    }
}

}  // namespace mongo::optimizer::cascades
