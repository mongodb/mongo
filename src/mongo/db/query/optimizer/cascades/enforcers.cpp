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

#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"
#include "mongo/util/assert_util.h"

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
                        PhysRewriteQueue& queue,
                        const PhysProps& physProps,
                        const LogicalProps& logicalProps,
                        PrefixId& prefixId)
        : _groupId(groupId),
          _metadata(metadata),
          _ridProjections(ridProjections),
          _queue(queue),
          _physProps(physProps),
          _logicalProps(logicalProps),
          _prefixId(prefixId) {}

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
            childProps, LimitEstimate{{static_cast<double>(prop.getAbsoluteLimit())}});

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

        const auto& requiredDistrAndProj = prop.getDistributionAndProjections();
        if (requiredDistrAndProj._type == DistributionType::UnknownPartitioning) {
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

        const auto& availableDistrs =
            getPropertyConst<DistributionAvailability>(_logicalProps).getDistributionSet();
        for (const auto& availableDistr : availableDistrs) {
            if (availableDistr == requiredDistrAndProj) {
                // Same distribution.
                continue;
            }
            if (availableDistr._type == DistributionType::Replicated) {
                // Cannot switch "away" from replicated distribution.
                continue;
            }

            PhysProps childProps = _physProps;
            setPropertyOverwrite<DistributionRequirement>(childProps, availableDistr);

            addProjectionsToProperties(childProps, requiredDistrAndProj._projectionNames);
            getProperty<DistributionRequirement>(childProps).setDisableExchanges(true);

            ABT enforcer = make<ExchangeNode>(prop, make<MemoLogicalDelegatorNode>(_groupId));
            optimizeChild<ExchangeNode, PhysicalRewriteType::EnforceDistribution>(
                _queue, kDefaultPriority, std::move(enforcer), std::move(childProps));
        }
    }

    void operator()(const PhysProperty&, const ProjectionRequirement& prop) {
        const ProjectionNameSet& availableProjections =
            getPropertyConst<ProjectionAvailability>(_logicalProps).getProjections();

        boost::optional<ProjectionName> ridProjName;
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

    void operator()(const PhysProperty&, const IndexingRequirement& prop) {}

    void operator()(const PhysProperty&, const RepetitionEstimate& prop) {
        // Noop. We do not currently enforce this property. It only affects costing.
        // TODO: consider materializing the subtree if we estimate a lot of repetitions.
    }

    void operator()(const PhysProperty&, const LimitEstimate& prop) {
        // Noop. We do not currently enforce this property. It only affects costing.
    }

    void operator()(const PhysProperty&, const RemoveOrphansRequirement& prop) {
        if (!prop.mustRemove()) {
            // Nothing to do if we don't need to remove any orphans.
            return;
        }

        tassert(7829701,
                "Enforcer for RemoveOrphansRequirement for a group without IndexingAvailability",
                hasProperty<IndexingAvailability>(_logicalProps));

        const auto& scanDefName =
            getPropertyConst<IndexingAvailability>(_logicalProps).getScanDefName();
        const auto& scanDef = _metadata._scanDefs.at(scanDefName);

        // Constuct a plan fragment which enforces the requirement by projecting all fields of the
        // shard key and invoking the shardFilter FunctionCall in a filter.
        const auto& shardKey = scanDef.shardingMetadata().shardKey();
        tassert(
            7829702,
            "Enforcer for RemoveOrphansRequirement but scan definition doesn't have a shard key.",
            !shardKey.empty());
        const auto& scanProj =
            getPropertyConst<IndexingAvailability>(_logicalProps).getScanProjection();
        ABTVector shardKeyFieldVars;

        PhysPlanBuilder builder{make<MemoLogicalDelegatorNode>(_groupId)};

        // Use the cardinality estimate of the group for costing purposes of the evaluation and
        // filter nodes that we are constructing in this plan fragment because in the majority of
        // cases, we expect there to be very few orphans and thus we don't adjust CE estimates to
        // account for them.
        auto ce = getPropertyConst<CardinalityEstimate>(_logicalProps);

        // Save a pointer to the MemoLogicalDelagatorNode so we can use it in the childPropsMap.
        ABT* childPtr = nullptr;

        for (auto&& fieldSpec : shardKey) {
            auto projName = _prefixId.getNextId("shardKey");
            builder.make<EvaluationNode>(ce.getEstimate(),
                                         projName,
                                         make<EvalPath>(fieldSpec._path, make<Variable>(scanProj)),
                                         std::move(builder._node));
            ABT shardKeyFieldVar = make<Variable>(std::move(projName));
            if (fieldSpec._op == CollationOp::Clustered) {
                shardKeyFieldVar =
                    make<FunctionCall>("shardHash", makeSeq(std::move(shardKeyFieldVar)));
            }
            shardKeyFieldVars.push_back(std::move(shardKeyFieldVar));
            if (childPtr == nullptr) {
                childPtr = &builder._node.cast<EvaluationNode>()->getChild();
            }
        }
        tassert(7829703, "Unable to save pointer to MemoLogicalDelagatorNode child.", childPtr);
        builder.make<FilterNode>(ce.getEstimate(),
                                 make<FunctionCall>("shardFilter", std::move(shardKeyFieldVars)),
                                 std::move(builder._node));

        PhysProps childProps = _physProps;
        setPropertyOverwrite(childProps, RemoveOrphansRequirement{false});
        addProjectionsToProperties(childProps, ProjectionNameSet{scanProj});

        ChildPropsType childPropsMap;
        childPropsMap.emplace_back(childPtr, std::move(childProps));

        optimizeChildrenNoAssert(_queue,
                                 kDefaultPriority,
                                 PhysicalRewriteType::EnforceShardFilter,
                                 std::move(builder._node),
                                 std::move(childPropsMap),
                                 std::move(builder._nodeCEMap));
    }

private:
    const GroupIdType _groupId;

    // We don't own any of those.
    const Metadata& _metadata;
    const RIDProjectionsMap& _ridProjections;
    PhysRewriteQueue& _queue;
    const PhysProps& _physProps;
    const LogicalProps& _logicalProps;
    PrefixId& _prefixId;
};

void addEnforcers(const GroupIdType groupId,
                  const Metadata& metadata,
                  const RIDProjectionsMap& ridProjections,
                  PhysRewriteQueue& queue,
                  const PhysProps& physProps,
                  const LogicalProps& logicalProps,
                  PrefixId& prefixId) {
    PropEnforcerVisitor visitor(
        groupId, metadata, ridProjections, queue, physProps, logicalProps, prefixId);
    for (const auto& entry : physProps) {
        entry.second.visit(visitor);
    }
}

}  // namespace mongo::optimizer::cascades
