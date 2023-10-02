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

#include "mongo/db/query/ce/sampling_estimator.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/sel_tree_utils.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/partial_schema_requirements.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/physical_plan_builder.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer::ce {
class SamplingPlanExtractor {
public:
    SamplingPlanExtractor(const cascades::Memo& memo,
                          const OptPhaseManager& phaseManager,
                          const size_t sampleSize)
        : _memo(memo), _sampleSize(sampleSize), _phaseManager(phaseManager) {}

    void transport(ABT& n, const MemoLogicalDelegatorNode& node) {
        n = extract(_memo.getLogicalNodes(node.getGroupId()).front());
    }

    void transport(ABT& n, const ScanNode& /*node*/, ABT& /*binder*/) {
        // We will lower the scan node in a sampling context here.
        // TODO: for now just return the documents in random order.
        n = make<LimitSkipNode>(properties::LimitSkipRequirement(_sampleSize, 0), std::move(n));
    }

    void transport(ABT& n, const FilterNode& /*node*/, ABT& childResult, ABT& /*exprResult*/) {
        // Skip over filters.
        n = childResult;
    }

    void transport(ABT& /*n*/,
                   const EvaluationNode& /*node*/,
                   ABT& /*childResult*/,
                   ABT& /*exprResult*/) {
        // Keep Eval nodes.
    }

    void transport(ABT& n, const SargableNode& node, ABT& childResult, ABT& refs, ABT& binds) {
        // We don't need to estimate cardinality of the sampling query itself, so the NodeCEMap part
        // is ignored here. We use a builder only because lowerPartialSchemaRequirement requires
        // one.
        PhysPlanBuilder result{childResult};

        // Retain only output bindings without applying filters.
        PSRExpr::visitAnyShape(
            node.getReqMap(), [&](const PartialSchemaEntry& e, const PSRExpr::VisitorContext& ctx) {
                const auto& [key, req] = e;
                if (const auto& boundProjName = req.getBoundProjectionName()) {
                    lowerPartialSchemaRequirement(
                        key,
                        PartialSchemaRequirement{
                            boundProjName, IntervalReqExpr::makeSingularDNF(), req.getIsPerfOnly()},
                        _phaseManager.getPathToInterval(),
                        boost::none /*residualCE*/,
                        result);
                }
            });
        std::swap(n, result._node);
    }

    void transport(ABT& n, const CollationNode& /*node*/, ABT& childResult, ABT& refs) {
        // Skip over collation nodes.
        n = childResult;
    }

    template <typename T, typename... Ts>
    void transport(ABT& /*n*/, const T& /*node*/, Ts&&...) {
        if constexpr (std::is_base_of_v<Node, T>) {
            uasserted(6624242, "Should not be seeing other types of nodes here.");
        }
    }

    ABT extract(ABT node) {
        algebra::transport<true>(node, *this);
        return node;
    }

private:
    const cascades::Memo& _memo;
    const size_t _sampleSize;
    const OptPhaseManager& _phaseManager;
};

class SamplingChunksTransport {

public:
    SamplingChunksTransport(NodeToGroupPropsMap& propsMap, const OptPhaseManager& phaseManager)
        : _propsMap(propsMap), _phaseManager(phaseManager) {}

    void transport(ABT& n, const LimitSkipNode& limit, ABT& child) {
        if (limit.getProperty().getSkip() != 0) {
            return;
        }
        const PhysicalScanNode& physicalScan = *child.cast<PhysicalScanNode>();
        const auto& ridProj = _phaseManager.getRIDProjections().at(physicalScan.getScanDefName());

        ABT newPhysicalScan =
            make<PhysicalScanNode>(FieldProjectionMap{._ridProjection = ProjectionName{ridProj}},
                                   physicalScan.getScanDefName(),
                                   physicalScan.useParallelScan());

        NodeProps props = _propsMap.at(&physicalScan);
        properties::getProperty<properties::ProjectionRequirement>(props._physicalProps)
            .getProjections() = ProjectionNameVector{ridProj};
        _propsMap.emplace(newPhysicalScan.cast<Node>(), props);

        ABT seekNode = make<SeekNode>(
            ridProj, physicalScan.getFieldProjectionMap(), physicalScan.getScanDefName());
        _propsMap.emplace(seekNode.cast<Node>(), props);

        const int32_t limitSize = limit.getProperty().getLimit();
        const int32_t numChunks = std::min(_phaseManager.getHints()._numSamplingChunks, limitSize);
        const int32_t chunkSize = limitSize / numChunks;

        ABT outerNode = make<LimitSkipNode>(properties::LimitSkipRequirement(numChunks, 0),
                                            std::move(newPhysicalScan));
        ABT innerNode = make<LimitSkipNode>(properties::LimitSkipRequirement(chunkSize, 0),
                                            std::move(seekNode));

        const NodeProps& limitProps = _propsMap.at(n.cast<Node>());
        NodeProps sharedProps = NodeProps{limitProps._planNodeId,
                                          limitProps._groupId,
                                          limitProps._logicalProps,
                                          limitProps._physicalProps,
                                          boost::none /*ridProjName*/,
                                          CostType::fromDouble(0),
                                          CostType::fromDouble(0),
                                          true /*adjustedCE*/};

        NodeProps outerLimitProps = sharedProps;
        properties::getProperty<properties::ProjectionRequirement>(outerLimitProps._physicalProps)
            .getProjections() = ProjectionNameVector{ridProj};
        _propsMap.emplace(outerNode.cast<Node>(), outerLimitProps);

        _propsMap.emplace(innerNode.cast<Node>(), sharedProps);

        ABT nlj = make<NestedLoopJoinNode>(JoinType::Inner,
                                           ProjectionNameSet{ridProj},
                                           Constant::boolean(true),
                                           std::move(outerNode),
                                           std::move(innerNode));

        _propsMap.emplace(nlj.cast<Node>(), sharedProps);
        std::swap(n, nlj);
    }

    /**
     * Template to handle all other cases - we don't care or need to do anything here, so we
     * knock out all the other required implementations at once with this template.
     */
    template <typename T, typename... Args>
    void transport(ABT& n, const T& node, Args&&... args) {
        static_assert(!std::is_same_v<T, LimitSkipNode>, "Missing LimitSkip handler");
        return;
    }

private:
    NodeToGroupPropsMap& _propsMap;
    const OptPhaseManager& _phaseManager;
};

class SamplingTransport {
    static constexpr size_t kMaxSampleSize = 1000;

public:
    SamplingTransport(OperationContext* opCtx,
                      OptPhaseManager phaseManager,
                      const int64_t numRecords,
                      std::unique_ptr<cascades::CardinalityEstimator> fallbackCE)
        : _phaseManager(std::move(phaseManager)),
          _opCtx(opCtx),
          _sampleSize(std::min<int64_t>(numRecords, kMaxSampleSize)),
          _fallbackCE(std::move(fallbackCE)) {}

    CEType transport(const ABT::reference_type n,
                     const FilterNode& node,
                     const Metadata& metadata,
                     const cascades::Memo& memo,
                     const properties::LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*exprResult*/) {
        if (!properties::hasProperty<properties::IndexingAvailability>(logicalProps)) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, n);
        }

        SamplingPlanExtractor planExtractor(memo, _phaseManager, _sampleSize);
        // Create a plan with all eval nodes so far and the filter last.
        ABT abtTree = make<FilterNode>(node.getFilter(), planExtractor.extract(n.copy()));

        return estimateFilterCE(metadata, memo, logicalProps, n, std::move(abtTree), childResult);
    }

    CEType transport(const ABT::reference_type n,
                     const SargableNode& node,
                     const Metadata& metadata,
                     const cascades::Memo& memo,
                     const properties::LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*bindResult*/,
                     CEType /*refsResult*/) {
        if (!properties::hasProperty<properties::IndexingAvailability>(logicalProps)) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, n);
        }

        SamplingPlanExtractor planExtractor(memo, _phaseManager, _sampleSize);
        ABT extracted = planExtractor.extract(n.copy());

        // Estimate individual requirements separately by potentially re-using cached results.
        // TODO: consider estimating together the entire set of requirements (but caching!)
        EstimatePartialSchemaEntrySelFn estimateFn = [&](SelectivityTreeBuilder& selTreeBuilder,
                                                         const PartialSchemaEntry& e) {
            const auto& [key, req] = e;

            if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                PhysPlanBuilder lowered{extracted};
                // Lower requirement without an output binding.
                lowerPartialSchemaRequirement(
                    key,
                    PartialSchemaRequirement{boost::none /*boundProjectionName*/,
                                             req.getIntervals(),
                                             req.getIsPerfOnly()},
                    _phaseManager.getPathToInterval(),
                    boost::none /*residualCE*/,
                    lowered);
                uassert(6624243, "Expected a filter node", lowered._node.is<FilterNode>());
                const CEType filterCE = estimateFilterCE(
                    metadata, memo, logicalProps, n, std::move(lowered._node), childResult);
                const SelectivityType sel =
                    childResult > 0.0 ? (filterCE / childResult) : SelectivityType{0.0};
                selTreeBuilder.atom(sel);
            }
        };

        PartialSchemaRequirementsCardinalityEstimator estimator(estimateFn, childResult);
        return estimator.estimateCE(node.getReqMap());
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    CEType transport(ABT::reference_type n,
                     const T& /*node*/,
                     const Metadata& metadata,
                     const cascades::Memo& memo,
                     const properties::LogicalProps& logicalProps,
                     Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, n);
        }
        return {0.0};
    }

    CEType derive(const Metadata& metadata,
                  const cascades::Memo& memo,
                  const properties::LogicalProps& logicalProps,
                  const ABT::reference_type logicalNodeRef) {
        return algebra::transport<true>(logicalNodeRef, *this, metadata, memo, logicalProps);
    }

private:
    CEType estimateFilterCE(const Metadata& metadata,
                            const cascades::Memo& memo,
                            const properties::LogicalProps& logicalProps,
                            const ABT::reference_type n,
                            ABT abtTree,
                            CEType childResult) {
        auto it = _selectivityCacheMap.find(abtTree);
        if (it != _selectivityCacheMap.cend()) {
            // Cache hit.
            return it->second * childResult;
        }

        const auto selectivity = estimateSelectivity(abtTree);
        if (!selectivity) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, n);
        }

        _selectivityCacheMap.emplace(std::move(abtTree), *selectivity);

        OPTIMIZER_DEBUG_LOG(6264805,
                            5,
                            "CE sampling estimated filter selectivity",
                            "selectivity"_attr = selectivity->_value);
        return *selectivity * childResult;
    }

    boost::optional<optimizer::SelectivityType> estimateSelectivity(ABT abt) {
        // Add a group by to count number of documents.
        const ProjectionName sampleSumProjection = "sum";
        abt = make<GroupByNode>(ProjectionNameVector{},
                                ProjectionNameVector{sampleSumProjection},
                                makeSeq(make<FunctionCall>("$sum", makeSeq(Constant::int64(1)))),
                                std::move(abt));
        abt = make<RootNode>(
            properties::ProjectionRequirement{ProjectionNameVector{sampleSumProjection}},
            std::move(abt));


        OPTIMIZER_DEBUG_LOG(6264806,
                            5,
                            "Estimate selectivity ABT",
                            "explain"_attr = ExplainGenerator::explainV2(abt));

        PlanAndProps planAndProps = _phaseManager.optimizeAndReturnProps(std::move(abt));

        // If internalCascadesOptimizerSampleChunks is a positive integer, sample by chunks using
        // that value as the number of chunks. Otherwise perform fully randomized sample.
        if (_phaseManager.getHints()._numSamplingChunks > 0) {
            SamplingChunksTransport instance{planAndProps._map, _phaseManager};
            algebra::transport<true>(planAndProps._node, instance);

            OPTIMIZER_DEBUG_LOG(6264807,
                                5,
                                "Physical Sampling",
                                "explain"_attr = ExplainGenerator::explainV2(planAndProps._node));
        }

        auto env = VariableEnvironment::build(planAndProps._node);
        SlotVarMap slotMap;
        auto runtimeEnvironment = std::make_unique<sbe::RuntimeEnvironment>();  // TODO Use factory
        boost::optional<sbe::value::SlotId> ridSlot;
        sbe::value::SlotIdGenerator ids;
        SBENodeLowering g{env,
                          *runtimeEnvironment,
                          ids,
                          _phaseManager.getMetadata(),
                          planAndProps._map,
                          internalCascadesOptimizerSamplingCEScanStartOfColl.load()
                              ? ScanOrder::Forward
                              : ScanOrder::Random};
        auto sbePlan = g.optimize(planAndProps._node, slotMap, ridSlot);
        tassert(6624261, "Unexpected rid slot", !ridSlot);

        // TODO: return errors instead of exceptions?
        uassert(6624244, "Lowering failed", sbePlan != nullptr);
        uassert(6624245, "Invalid slot map size", slotMap.size() == 1);

        sbePlan->attachToOperationContext(_opCtx);
        sbe::CompileCtx ctx(std::move(runtimeEnvironment));
        sbePlan->prepare(ctx);

        std::vector<sbe::value::SlotAccessor*> accessors;
        for (auto& [name, slot] : slotMap) {
            accessors.emplace_back(sbePlan->getAccessor(ctx, slot));
        }

        sbePlan->open(false);
        ON_BLOCK_EXIT([&] { sbePlan->close(); });

        while (sbePlan->getNext() != sbe::PlanState::IS_EOF) {
            const auto [tag, value] = accessors.at(0)->getViewOfValue();
            if (tag == sbe::value::TypeTags::NumberInt64) {
                // TODO: check if we get exactly one result from the groupby?
                return {{static_cast<double>(value) / _sampleSize}};
            }
            return boost::none;
        };

        // If nothing passes the filter, estimate 0.0 selectivity. HashGroup will return 0 results.
        return {{0.0}};
    }

    struct NodeRefHash {
        size_t operator()(const ABT& node) const {
            return ABTHashGenerator::generate(node);
        }
    };

    struct NodeRefCompare {
        bool operator()(const ABT& left, const ABT& right) const {
            return left == right;
        }
    };

    // Cache a logical node reference to computed selectivity. Used for Filter and Sargable nodes.
    opt::unordered_map<ABT, SelectivityType, NodeRefHash, NodeRefCompare> _selectivityCacheMap;

    OptPhaseManager _phaseManager;

    // We don't own this.
    OperationContext* _opCtx;

    const int64_t _sampleSize;
    std::unique_ptr<cascades::CardinalityEstimator> _fallbackCE;
};

SamplingEstimator::SamplingEstimator(OperationContext* opCtx,
                                     OptPhaseManager phaseManager,
                                     const int64_t numRecords,
                                     std::unique_ptr<cascades::CardinalityEstimator> fallbackCE)
    : _transport(std::make_unique<SamplingTransport>(
          opCtx, std::move(phaseManager), numRecords, std::move(fallbackCE))) {}

SamplingEstimator::~SamplingEstimator() {}

CEType SamplingEstimator::deriveCE(const Metadata& metadata,
                                   const cascades::Memo& memo,
                                   const properties::LogicalProps& logicalProps,
                                   const ABT::reference_type logicalNodeRef) const {
    return _transport->derive(metadata, memo, logicalProps, logicalNodeRef);
}

}  // namespace mongo::optimizer::ce
