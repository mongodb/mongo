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

#include "mongo/db/query/optimizer/cascades/enforcers.h"

#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"

namespace mongo::optimizer::cascades {

using namespace properties;

// Maximum Limit to consider to implement in the sort stage (via a min-heap internally).
static constexpr int64_t kMaxLimitForSort = 100;

static bool isDistributionCentralizedOrReplicated(const PhysProps& physProps) {
    switch (getPropertyConst<DistributionRequirement>(physProps)
                .getDistributionAndProjections()
                ._type) {
        case DistributionType::Centralized:
        case DistributionType::Replicated:
            return true;

        default:
            return false;
    }
}

/**
 * Checks if we are not trying to satisfy using the entire collection. We are either aiming for a
 * covered index, or for a seek.
 */
static bool hasIncompleteScanIndexingRequirement(const PhysProps& physProps) {
    return hasProperty<IndexingRequirement>(physProps) &&
        getPropertyConst<IndexingRequirement>(physProps).getIndexReqTarget() !=
        IndexReqTarget::Complete;
}

class PropEnforcerVisitor {
public:
    PropEnforcerVisitor(const GroupIdType groupId,
                        const Metadata& metadata,
                        const RIDProjectionsMap& ridProjections,
                        PrefixId& prefixId,
                        PhysRewriteQueue& queue,
                        const PhysProps& physProps,
                        const LogicalProps& logicalProps)
        : _groupId(groupId),
          _metadata(metadata),
          _ridProjections(ridProjections),
          _prefixId(prefixId),
          _queue(queue),
          _physProps(physProps),
          _logicalProps(logicalProps) {}

    void operator()(const PhysProperty&, const CollationRequirement& prop) {
        if (hasIncompleteScanIndexingRequirement(_physProps)) {
            // If we have indexing requirements, we do not enforce collation separately.
            // It will be satisfied as part of the index collation.
            return;
        }

        PhysProps childProps = _physProps;
        removeProperty<CollationRequirement>(childProps);
        addProjectionsToProperties(childProps, prop.getAffectedProjectionNames());

        // TODO: also remove RepetitionEstimate if the subtree does not use bound variables.
        removeProperty<LimitEstimate>(childProps);

        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            const auto& limitSkipReq = getPropertyConst<LimitSkipRequirement>(_physProps);
            if (prop.hasClusteredOp() || limitSkipReq.getSkip() != 0 ||
                limitSkipReq.getLimit() > kMaxLimitForSort) {
                // We cannot enforce collation+skip or collation+large limit.
                return;
            }

            // We can satisfy both collation and limit-skip requirement. During lowering, physical
            // properties will indicate presence of limit skip, and thus we set the limit on the sbe
            // stage.
            removeProperty<LimitSkipRequirement>(childProps);
        }

        ABT enforcer = make<CollationNode>(prop, make<MemoLogicalDelegatorNode>(_groupId));
        optimizeChild<CollationNode, PhysicalRewriteType::EnforceCollation>(
            _queue, kDefaultPriority, std::move(enforcer), std::move(childProps));
    }

    void operator()(const PhysProperty&, const LimitSkipRequirement& prop) {
        if (hasIncompleteScanIndexingRequirement(_physProps)) {
            // If we have indexing requirements, we do not enforce limit skip.
            return;
        }
        if (!isDistributionCentralizedOrReplicated(_physProps)) {
            // Can only enforce limit-skip under centralized or replicated distribution.
            return;
        }

        PhysProps childProps = _physProps;
        removeProperty<LimitSkipRequirement>(childProps);
        setPropertyOverwrite<LimitEstimate>(
            childProps, LimitEstimate{static_cast<CEType>(prop.getAbsoluteLimit())});

        ABT enforcer = make<LimitSkipNode>(prop, make<MemoLogicalDelegatorNode>(_groupId));
        optimizeChild<LimitSkipNode, PhysicalRewriteType::EnforceLimitSkip>(
            _queue, kDefaultPriority, std::move(enforcer), std::move(childProps));
    }

    void operator()(const PhysProperty&, const DistributionRequirement& prop) {
        if (!_metadata.isParallelExecution()) {
            // We're running in serial mode.
            return;
        }
        if (prop.getDisableExchanges()) {
            // We cannot change distributions.
            return;
        }
        if (hasProperty<IndexingRequirement>(_physProps) &&
            getPropertyConst<IndexingRequirement>(_physProps).getIndexReqTarget() ==
                IndexReqTarget::Seek) {
            // Cannot change distributions while under Seek requirement.
            return;
        }

        if (prop.getDistributionAndProjections()._type == DistributionType::UnknownPartitioning) {
            // Cannot exchange into unknown partitioning.
            return;
        }

        // TODO: consider hash partition on RID if under IndexingAvailability.

        const bool hasCollation = hasProperty<CollationRequirement>(_physProps);
        if (hasCollation) {
            // For now we cannot enforce if we have collation requirement.
            // TODO: try enforcing into partitioning distributions which form prefixes over the
            // collation, with ordered exchange.
            return;
        }

        const auto& distributions =
            getPropertyConst<DistributionAvailability>(_logicalProps).getDistributionSet();
        for (const auto& distribution : distributions) {
            if (distribution == prop.getDistributionAndProjections()) {
                // Same distribution.
                continue;
            }
            if (distribution._type == DistributionType::Replicated) {
                // Cannot switch "away" from replicated distribution.
                continue;
            }

            PhysProps childProps = _physProps;
            setPropertyOverwrite<DistributionRequirement>(childProps, distribution);

            addProjectionsToProperties(childProps, distribution._projectionNames);
            getProperty<DistributionRequirement>(childProps).setDisableExchanges(true);

            ABT enforcer = make<ExchangeNode>(prop, make<MemoLogicalDelegatorNode>(_groupId));
            optimizeChild<ExchangeNode, PhysicalRewriteType::EnforceDistribution>(
                _queue, kDefaultPriority, std::move(enforcer), std::move(childProps));
        }
    }

    void operator()(const PhysProperty&, const ProjectionRequirement& prop) {
        const ProjectionNameSet& availableProjections =
            getPropertyConst<ProjectionAvailability>(_logicalProps).getProjections();

        ProjectionName ridProjName;
        if (hasProperty<IndexingAvailability>(_logicalProps)) {
            const auto& scanDefName =
                getPropertyConst<IndexingAvailability>(_logicalProps).getScanDefName();
            ridProjName = _ridProjections.at(scanDefName);
        }

        // Verify we can satisfy the required projections using the logical projections, or the rid
        // projection if we have indexing availability.
        for (const ProjectionName& projectionName : prop.getProjections().getVector()) {
            if (projectionName != ridProjName &&
                availableProjections.find(projectionName) == availableProjections.cend()) {
                uasserted(6624100, "Cannot satisfy all projections");
            }
        }
    }

    void operator()(const PhysProperty&, const IndexingRequirement& prop) {
        if (prop.getIndexReqTarget() != IndexReqTarget::Complete) {
            return;
        }

        uassert(6624101,
                "IndexingRequirement without indexing availability",
                hasProperty<IndexingAvailability>(_logicalProps));
        const IndexingAvailability& indexingAvailability =
            getPropertyConst<IndexingAvailability>(_logicalProps);

        // TODO: consider left outer joins. We can propagate rid from the outer side.
        if (_metadata._scanDefs.at(indexingAvailability.getScanDefName()).getIndexDefs().empty()) {
            // No indexes on the collection.
            return;
        }

        const ProjectionNameOrderPreservingSet& requiredProjections =
            getPropertyConst<ProjectionRequirement>(_physProps).getProjections();
        const ProjectionName& scanProjection = indexingAvailability.getScanProjection();
        const bool requiresScanProjection = requiredProjections.find(scanProjection).second;

        if (!requiresScanProjection) {
            // Try indexScanOnly (covered index) if we do not require scan projection.
            PhysProps newProps = _physProps;
            setPropertyOverwrite<IndexingRequirement>(newProps,
                                                      {IndexReqTarget::Index,
                                                       prop.getDedupRID(),
                                                       prop.getSatisfiedPartialIndexesGroupId()});

            optimizeUnderNewProperties<PhysicalRewriteType::AttemptCoveringQuery>(
                _queue,
                kDefaultPriority,
                make<MemoLogicalDelegatorNode>(_groupId),
                std::move(newProps));
        }
    }

    void operator()(const PhysProperty&, const RepetitionEstimate& prop) {
        // Noop. We do not currently enforce this property. It only affects costing.
        // TODO: consider materializing the subtree if we estimate a lot of repetitions.
    }

    void operator()(const PhysProperty&, const LimitEstimate& prop) {
        // Noop. We do not currently enforce this property. It only affects costing.
    }

private:
    const GroupIdType _groupId;

    // We don't own any of those.
    const Metadata& _metadata;
    const RIDProjectionsMap& _ridProjections;
    PrefixId& _prefixId;
    PhysRewriteQueue& _queue;
    const PhysProps& _physProps;
    const LogicalProps& _logicalProps;
};

void addEnforcers(const GroupIdType groupId,
                  const Metadata& metadata,
                  const RIDProjectionsMap& ridProjections,
                  PrefixId& prefixId,
                  PhysRewriteQueue& queue,
                  const PhysProps& physProps,
                  const LogicalProps& logicalProps) {
    PropEnforcerVisitor visitor(
        groupId, metadata, ridProjections, prefixId, queue, physProps, logicalProps);
    for (const auto& entry : physProps) {
        entry.second.visit(visitor);
    }
}

}  // namespace mongo::optimizer::cascades
