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

#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer::cascades {

using namespace properties;

static void populateInitialDistributions(const DistributionAndPaths& distributionAndPaths,
                                         const bool isMultiPartition,
                                         DistributionSet& distributions) {
    switch (distributionAndPaths._type) {
        case DistributionType::Centralized:
            distributions.insert({DistributionType::Centralized});
            break;

        case DistributionType::Replicated:
            uassert(6624106, "Invalid distribution specification", isMultiPartition);

            distributions.insert({DistributionType::Centralized});
            distributions.insert({DistributionType::Replicated});
            break;

        case DistributionType::HashPartitioning:
        case DistributionType::RangePartitioning:
        case DistributionType::UnknownPartitioning:
            uassert(6624107, "Invalid distribution specification", isMultiPartition);

            distributions.insert({DistributionType::UnknownPartitioning});
            break;

        default:
            uasserted(6624108, "Invalid collection distribution");
    }
}

static void populateDistributionPaths(const PartialSchemaRequirements& req,
                                      const ProjectionName& scanProjectionName,
                                      const DistributionAndPaths& distributionAndPaths,
                                      DistributionSet& distributions) {
    switch (distributionAndPaths._type) {
        case DistributionType::HashPartitioning:
        case DistributionType::RangePartitioning: {
            ProjectionNameVector distributionProjections;

            for (const ABT& path : distributionAndPaths._paths) {
                auto it = req.find(PartialSchemaKey{scanProjectionName, path});
                if (it == req.cend()) {
                    break;
                }
                if (it->second.hasBoundProjectionName()) {
                    distributionProjections.push_back(it->second.getBoundProjectionName());
                }
            }

            if (distributionProjections.size() == distributionAndPaths._paths.size()) {
                distributions.emplace(distributionAndPaths._type,
                                      std::move(distributionProjections));
            }
            break;
        }

        default:
            break;
    }
}

static bool computeEqPredsOnly(const PartialSchemaRequirements& reqMap) {
    PartialSchemaRequirements equalitiesReqMap;
    PartialSchemaRequirements fullyOpenReqMap;

    for (const auto& [key, req] : reqMap) {
        const auto& intervals = req.getIntervals();
        if (auto singularInterval = IntervalReqExpr::getSingularDNF(intervals)) {
            if (singularInterval->isFullyOpen()) {
                fullyOpenReqMap.emplace(key, req);
            } else if (singularInterval->isEquality()) {
                equalitiesReqMap.emplace(key, req);
            } else {
                // Encountered a non-equality and not-fully-open interval.
                return false;
            }
        } else {
            // Encountered a non-trivial interval.
            return false;
        }
    }

    for (const auto& [key, req] : fullyOpenReqMap) {
        if (equalitiesReqMap.count(key) == 0) {
            // No possible match for fully open requirement.
            return false;
        }
    }

    return true;
}

class DeriveLogicalProperties {
public:
    LogicalProps transport(const ScanNode& node, LogicalProps /*bindResult*/) {
        DistributionSet distributions;

        const auto& scanDef = _metadata._scanDefs.at(node.getScanDefName());
        populateInitialDistributions(
            scanDef.getDistributionAndPaths(), _metadata.isParallelExecution(), distributions);
        for (const auto& entry : scanDef.getIndexDefs()) {
            populateInitialDistributions(entry.second.getDistributionAndPaths(),
                                         _metadata.isParallelExecution(),
                                         distributions);
        }

        return maybeUpdateNodePropsMap(node,
                                       createInitialScanProps(node.getProjectionName(),
                                                              node.getScanDefName(),
                                                              _groupId,
                                                              std::move(distributions)));
    }

    LogicalProps transport(const ValueScanNode& node, LogicalProps /*bindResult*/) {
        LogicalProps result;
        if (const auto& props = node.getProps(); props) {
            result = *props;
            if (hasProperty<IndexingAvailability>(result)) {
                // Update the group to our current one.
                getProperty<IndexingAvailability>(result).setScanGroupId(_groupId);
            }
        } else {
            // We do not originate indexing availability, and have empty collection availability
            // with Centralized + Replicated distribution availability. During physical optimization
            // we accept optimization under any distribution.
            result = makeLogicalProps(CollectionAvailability{{}}, DistributionAvailability{{}});
        }

        addCentralizedAndRoundRobinDistributions(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const MemoLogicalDelegatorNode& node) {
        uassert(6624109, "Uninitialized memo", _memo != nullptr);
        return maybeUpdateNodePropsMap(node, _memo->getGroup(node.getGroupId())._logicalProperties);
    }

    LogicalProps transport(const FilterNode& node,
                           LogicalProps childResult,
                           LogicalProps /*exprResult*/) {
        // Propagate indexing, collection, and distribution availabilities.
        LogicalProps result = std::move(childResult);
        if (hasProperty<IndexingAvailability>(result)) {
            getProperty<IndexingAvailability>(result).setEqPredsOnly(false);
        }
        addCentralizedAndRoundRobinDistributions(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const EvaluationNode& node,
                           LogicalProps childResult,
                           LogicalProps /*exprResult*/) {
        // We are specifically not adding the node's projection to ProjectionAvailability here.
        // The logical properties already contains projection availability which is derived first
        // when the memo group is created.
        LogicalProps result = std::move(childResult);
        if (hasProperty<IndexingAvailability>(result)) {
            getProperty<IndexingAvailability>(result).setEqPredsOnly(false);
        }
        addCentralizedAndRoundRobinDistributions(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const SargableNode& node,
                           LogicalProps childResult,
                           LogicalProps /*bindsResult*/,
                           LogicalProps /*refsResult*/) {
        LogicalProps result = std::move(childResult);

        auto& indexingAvailability = getProperty<IndexingAvailability>(result);
        const ProjectionName& scanProjectionName = indexingAvailability.getScanProjection();
        const std::string& scanDefName = indexingAvailability.getScanDefName();
        const auto& scanDef = _metadata._scanDefs.at(scanDefName);

        auto& distributions = getProperty<DistributionAvailability>(result).getDistributionSet();
        addCentralizedAndRoundRobinDistributions(result);

        populateDistributionPaths(
            node.getReqMap(), scanProjectionName, scanDef.getDistributionAndPaths(), distributions);
        for (const auto& entry : scanDef.getIndexDefs()) {
            populateDistributionPaths(node.getReqMap(),
                                      scanProjectionName,
                                      entry.second.getDistributionAndPaths(),
                                      distributions);
        }

        if (indexingAvailability.getEqPredsOnly()) {
            indexingAvailability.setEqPredsOnly(computeEqPredsOnly(node.getReqMap()));
        }

        auto& satisfiedPartialIndexes =
            getProperty<IndexingAvailability>(result).getSatisfiedPartialIndexes();
        for (const auto& [indexDefName, indexDef] : scanDef.getIndexDefs()) {
            if (!indexDef.getPartialReqMap().empty()) {
                auto intersection = node.getReqMap();
                // We specifically ignore projectionRenames here.
                ProjectionRenames projectionRenames_unused;
                if (intersectPartialSchemaReq(
                        intersection, indexDef.getPartialReqMap(), projectionRenames_unused) &&
                    intersection == node.getReqMap()) {
                    satisfiedPartialIndexes.insert(indexDefName);
                }
            }
        }

        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const RIDIntersectNode& node,
                           LogicalProps /*leftChildResult*/,
                           LogicalProps /*rightChildResult*/) {
        // Properties for the group should already be derived via the underlying Filter or
        // Evaluation logical nodes.
        uasserted(6624042, "Should not be necessary to derive properties for RIDIntersectNode");
    }

    LogicalProps transport(const BinaryJoinNode& node,
                           LogicalProps leftChildResult,
                           LogicalProps rightChildResult,
                           LogicalProps /*exprResult*/) {
        // We are specifically not adding the node's projection to ProjectionAvailability here.
        // The logical properties already contains projection availability which is derived first
        // when the memo group is created.

        LogicalProps result = std::move(leftChildResult);
        auto& mergedScanDefs = getProperty<CollectionAvailability>(result).getScanDefSet();
        auto& mergedDistributionSet =
            getProperty<DistributionAvailability>(result).getDistributionSet();

        auto rightChildScanDefs =
            getProperty<CollectionAvailability>(rightChildResult).getScanDefSet();
        mergedScanDefs.merge(std::move(rightChildScanDefs));

        auto rightChildDistributionSet =
            getProperty<DistributionAvailability>(rightChildResult).getDistributionSet();
        mergedDistributionSet.merge(std::move(rightChildDistributionSet));

        removeProperty<IndexingAvailability>(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const UnionNode& node,
                           std::vector<LogicalProps> childResults,
                           LogicalProps bindResult,
                           LogicalProps refsResult) {
        uassert(6624044, "Unexpected empty child results for union node", !childResults.empty());

        // We are specifically not adding the node's projection to ProjectionAvailability here.
        // The logical properties already contains projection availability which is derived first
        // when the memo group is created.
        LogicalProps result = std::move(childResults[0]);
        auto& mergedScanDefs = getProperty<CollectionAvailability>(result).getScanDefSet();
        auto& mergedDistributionSet =
            getProperty<DistributionAvailability>(result).getDistributionSet();
        for (size_t childIdx = 1; childIdx < childResults.size(); childIdx++) {
            auto childScanDefs =
                getProperty<CollectionAvailability>(childResults[childIdx]).getScanDefSet();
            mergedScanDefs.merge(std::move(childScanDefs));

            // Only keep the distribution properties which are common across all children
            // distributions.
            const auto& childDistributionSet =
                getProperty<DistributionAvailability>(childResults[childIdx]).getDistributionSet();

            for (auto it = mergedDistributionSet.begin(); it != mergedDistributionSet.end(); it++) {
                if (childDistributionSet.find(*it) == childDistributionSet.end()) {
                    mergedDistributionSet.erase(it);
                }
            }
        }

        // Verify that there is at least one common distribution available.
        uassert(6624045, "No common distributions for union", !mergedDistributionSet.empty());

        removeProperty<IndexingAvailability>(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const GroupByNode& node,
                           LogicalProps childResult,
                           LogicalProps /*bindAggResult*/,
                           LogicalProps /*refsAggResult*/,
                           LogicalProps /*bindGbResult*/,
                           LogicalProps /*refsGbResult*/) {
        LogicalProps result = std::move(childResult);
        removeProperty<IndexingAvailability>(result);

        auto& distributions = getProperty<DistributionAvailability>(result).getDistributionSet();
        addCentralizedAndRoundRobinDistributions<false /*addRoundRobin*/>(distributions);

        if (_metadata.isParallelExecution() && node.getType() != GroupNodeType::Local) {
            distributions.erase({DistributionType::UnknownPartitioning});
            distributions.erase({DistributionType::RoundRobin});

            // We propagate hash and range partitioning only if we are global agg.
            const ProjectionNameVector& groupByProjections = node.getGroupByProjectionNames();
            if (!groupByProjections.empty()) {
                DistributionRequirement allowedRangePartitioning{
                    {DistributionType::RangePartitioning, groupByProjections}};
                for (auto it = distributions.begin(); it != distributions.end();) {
                    switch (it->_type) {
                        case DistributionType::HashPartitioning:
                            // Erase all hash partition distributions. New ones will be generated
                            // after.
                            distributions.erase(it++);
                            break;

                        case DistributionType::RangePartitioning:
                            // Retain only the range partition which contains the group by
                            // projections in the node order.
                            if (*it == allowedRangePartitioning.getDistributionAndProjections()) {
                                it++;
                            } else {
                                distributions.erase(it++);
                            }
                            break;

                        default:
                            it++;
                            break;
                    }
                }

                // Generate hash distributions using the power set of group-by projections.
                for (size_t mask = 1; mask < (1ull << groupByProjections.size()); mask++) {
                    ProjectionNameVector projectionNames;
                    for (size_t index = 0; index < groupByProjections.size(); index++) {
                        if ((mask & (1ull << index)) != 0) {
                            projectionNames.push_back(groupByProjections.at(index));
                        }
                    }
                    distributions.emplace(DistributionType::HashPartitioning,
                                          std::move(projectionNames));
                }
            }
        }

        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const UnwindNode& node,
                           LogicalProps childResult,
                           LogicalProps /*bindResult*/,
                           LogicalProps /*refsResult*/) {
        LogicalProps result = std::move(childResult);
        removeProperty<IndexingAvailability>(result);

        const ProjectionName& unwoundProjectionName = node.getProjectionName();
        auto& distributions = getProperty<DistributionAvailability>(result).getDistributionSet();
        addCentralizedAndRoundRobinDistributions(distributions);

        if (_metadata.isParallelExecution()) {
            for (auto it = distributions.begin(); it != distributions.end();) {
                switch (it->_type) {
                    case DistributionType::HashPartitioning:
                    case DistributionType::RangePartitioning: {
                        // Erase partitioned distributions which contain the projection to unwind.
                        bool containsProjection = false;
                        for (const ProjectionName& projectionName : it->_projectionNames) {
                            if (projectionName == unwoundProjectionName) {
                                containsProjection = true;
                                break;
                            }
                        }
                        if (containsProjection) {
                            distributions.erase(it);
                        }
                        it++;
                        break;
                    }

                    default:
                        it++;
                        break;
                }
            }
        }

        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const CollationNode& node,
                           LogicalProps childResult,
                           LogicalProps /*refsResult*/) {
        LogicalProps result = std::move(childResult);
        // We propagate indexing availability.

        addCentralizedAndRoundRobinDistributions<false /*addRoundRobin*/>(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const LimitSkipNode& node, LogicalProps childResult) {
        LogicalProps result = std::move(childResult);
        removeProperty<IndexingAvailability>(result);
        addCentralizedAndRoundRobinDistributions<false /*addRoundRobin*/>(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const ExchangeNode& node,
                           LogicalProps childResult,
                           LogicalProps /*refsResult*/) {
        LogicalProps result = std::move(childResult);
        removeProperty<IndexingAvailability>(result);
        addCentralizedAndRoundRobinDistributions<false /*addRoundRobin*/>(result);
        return maybeUpdateNodePropsMap(node, std::move(result));
    }

    LogicalProps transport(const RootNode& node,
                           LogicalProps childResult,
                           LogicalProps /*refsResult*/) {
        return maybeUpdateNodePropsMap(node, std::move(childResult));
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    LogicalProps transport(const T& /*node*/, Ts&&...) {
        static_assert(!canBeLogicalNode<T>(),
                      "Logical node must implement its logical property derivation.");
        return {};
    }

    static LogicalProps derive(const Metadata& metadata,
                               const ABT::reference_type nodeRef,
                               LogicalPropsInterface::NodePropsMap* nodePropsMap,
                               const Memo* memo,
                               const GroupIdType groupId) {
        DeriveLogicalProperties instance(memo, metadata, groupId, nodePropsMap);
        return algebra::transport<false>(nodeRef, instance);
    }

private:
    DeriveLogicalProperties(const Memo* memo,
                            const Metadata& metadata,
                            const GroupIdType groupId,
                            LogicalPropsInterface::NodePropsMap* nodePropsMap)
        : _groupId(groupId), _memo(memo), _metadata(metadata), _nodePropsMap(nodePropsMap) {}

    template <bool addRoundRobin = true>
    void addCentralizedAndRoundRobinDistributions(DistributionSet& distributions) {
        distributions.emplace(DistributionType::Centralized);
        if (addRoundRobin && _metadata.isParallelExecution()) {
            distributions.emplace(DistributionType::RoundRobin);
        }
    }

    template <bool addRoundRobin = true>
    void addCentralizedAndRoundRobinDistributions(LogicalProps& properties) {
        addCentralizedAndRoundRobinDistributions<addRoundRobin>(
            getProperty<DistributionAvailability>(properties).getDistributionSet());
    }

    LogicalProps maybeUpdateNodePropsMap(const Node& node, LogicalProps props) {
        if (_nodePropsMap != nullptr) {
            _nodePropsMap->emplace(&node, props);
        }
        return props;
    }

    const GroupIdType _groupId;

    // We don't own any of those.
    const Memo* _memo;
    const Metadata& _metadata;
    LogicalPropsInterface::NodePropsMap* _nodePropsMap;
};

properties::LogicalProps DefaultLogicalPropsDerivation::deriveProps(
    const Metadata& metadata,
    const ABT::reference_type nodeRef,
    LogicalPropsInterface::NodePropsMap* nodePropsMap,
    const Memo* memo,
    const GroupIdType groupId) const {
    return DeriveLogicalProperties::derive(metadata, nodeRef, nodePropsMap, memo, groupId);
}

}  // namespace mongo::optimizer::cascades
