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

#include "mongo/db/query/ce/ce_sampling.h"

#include "mongo/db/commands/cqf/cqf_command_utils.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer::cascades {

using namespace properties;

class SamplingPlanExtractor {
public:
    SamplingPlanExtractor(const Memo& memo, const size_t sampleSize)
        : _memo(memo), _sampleSize(sampleSize) {}

    void transport(ABT& n, const MemoLogicalDelegatorNode& node) {
        n = extract(_memo.getGroup(node.getGroupId())._logicalNodes.at(0));
    }

    void transport(ABT& n, const ScanNode& /*node*/, ABT& /*binder*/) {
        // We will lower the scan node in a sampling context here.
        // TODO: for now just return the documents in random order.
        n = make<LimitSkipNode>(LimitSkipRequirement(_sampleSize, 0), std::move(n));
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

    void transport(ABT& n, const SargableNode& /*node*/, ABT& childResult, ABT& refs, ABT& binds) {
        // Skip over sargable nodes.
        n = childResult;
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
    const Memo& _memo;
    const size_t _sampleSize;
};

class CESamplingTransportImpl {
    static constexpr size_t kMaxSampleSize = 1000;

public:
    CESamplingTransportImpl(OperationContext* opCtx,
                            OptPhaseManager& phaseManager,
                            const int64_t numRecords)
        : _opCtx(opCtx),
          _phaseManager(phaseManager),
          _heuristicCE(),
          _sampleSize(std::min<int64_t>(numRecords, kMaxSampleSize)) {}

    CEType transport(const ABT& n,
                     const FilterNode& node,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*exprResult*/) {
        if (!hasProperty<IndexingAvailability>(logicalProps)) {
            return _heuristicCE.deriveCE(memo, logicalProps, n.ref());
        }

        SamplingPlanExtractor planExtractor(memo, _sampleSize);
        // Create a plan with all eval nodes so far and the filter last.
        ABT abtTree = make<FilterNode>(node.getFilter(), planExtractor.extract(n));

        return estimateFilterCE(memo, logicalProps, n, std::move(abtTree), childResult);
    }

    CEType transport(const ABT& n,
                     const SargableNode& node,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*bindResult*/,
                     CEType /*refsResult*/) {
        if (!hasProperty<IndexingAvailability>(logicalProps)) {
            return _heuristicCE.deriveCE(memo, logicalProps, n.ref());
        }

        SamplingPlanExtractor planExtractor(memo, _sampleSize);
        ABT extracted = planExtractor.extract(n);

        // Estimate individual requirements separately by potentially re-using cached results.
        // Here we assume that each requirement is independent.
        // TODO: consider estimating together the entire set of requirements (but caching!)
        CEType result = childResult;
        for (const auto& [key, req] : node.getReqMap()) {
            if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                ABT lowered = extracted;
                lowerPartialSchemaRequirement(key, req, lowered);
                uassert(6624243, "Expected a filter node", lowered.is<FilterNode>());
                result = estimateFilterCE(memo, logicalProps, n, std::move(lowered), result);
            }
        }

        return result;
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    CEType transport(const ABT& n,
                     const T& /*node*/,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _heuristicCE.deriveCE(memo, logicalProps, n.ref());
        }
        return 0.0;
    }

    CEType derive(const Memo& memo,
                  const properties::LogicalProps& logicalProps,
                  const ABT::reference_type logicalNodeRef) {
        return algebra::transport<true>(logicalNodeRef, *this, memo, logicalProps);
    }

private:
    CEType estimateFilterCE(const Memo& memo,
                            const LogicalProps& logicalProps,
                            const ABT& n,
                            ABT abtTree,
                            CEType childResult) {
        auto it = _selectivityCacheMap.find(abtTree);
        if (it != _selectivityCacheMap.cend()) {
            // Cache hit.
            return it->second * childResult;
        }

        const auto [success, selectivity] = estimateSelectivity(abtTree);
        if (!success) {
            return _heuristicCE.deriveCE(memo, logicalProps, n.ref());
        }

        _selectivityCacheMap.emplace(std::move(abtTree), selectivity);

        OPTIMIZER_DEBUG_LOG(6264805,
                            5,
                            "CE sampling estimated filter selectivity",
                            "selectivity"_attr = selectivity);
        return selectivity * childResult;
    }

    std::pair<bool, SelectivityType> estimateSelectivity(ABT abtTree) {
        // Add a group by to count number of documents.
        const ProjectionName sampleSumProjection = "sum";
        abtTree =
            make<GroupByNode>(ProjectionNameVector{},
                              ProjectionNameVector{sampleSumProjection},
                              makeSeq(make<FunctionCall>("$sum", makeSeq(Constant::int64(1)))),
                              std::move(abtTree));
        abtTree = make<RootNode>(
            properties::ProjectionRequirement{ProjectionNameVector{sampleSumProjection}},
            std::move(abtTree));


        OPTIMIZER_DEBUG_LOG(6264806,
                            5,
                            "Estimate selectivity ABT",
                            "explain"_attr = ExplainGenerator::explainV2(abtTree));

        if (!_phaseManager.optimize(abtTree)) {
            return {false, {}};
        }

        auto env = VariableEnvironment::build(abtTree);
        SlotVarMap slotMap;
        sbe::value::SlotIdGenerator ids;
        SBENodeLowering g{env,
                          slotMap,
                          ids,
                          _phaseManager.getMetadata(),
                          _phaseManager.getNodeToGroupPropsMap(),
                          _phaseManager.getRIDProjections(),
                          true /*randomScan*/};
        auto sbePlan = g.optimize(abtTree);

        // TODO: return errors instead of exceptions?
        uassert(6624244, "Lowering failed", sbePlan != nullptr);
        uassert(6624245, "Invalid slot map size", slotMap.size() == 1);

        sbePlan->attachToOperationContext(_opCtx);
        sbe::CompileCtx ctx(std::make_unique<sbe::RuntimeEnvironment>());
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
                return {true, static_cast<double>(value) / _sampleSize};
            }
            return {false, {}};
        };

        // If nothing passes the filter, estimate 0.0 selectivity. HashGroup will return 0 results.
        return {true, 0.0};
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

    // We don't own those.
    OperationContext* _opCtx;
    OptPhaseManager& _phaseManager;

    HeuristicCE _heuristicCE;
    const int64_t _sampleSize;
};

CESamplingTransport::CESamplingTransport(OperationContext* opCtx,
                                         OptPhaseManager& phaseManager,
                                         const int64_t numRecords)
    : _impl(std::make_unique<CESamplingTransportImpl>(opCtx, phaseManager, numRecords)) {}

CESamplingTransport::~CESamplingTransport() {}

CEType CESamplingTransport::deriveCE(const Memo& memo,
                                     const LogicalProps& logicalProps,
                                     const ABT::reference_type logicalNodeRef) const {
    return _impl->derive(memo, logicalProps, logicalNodeRef);
}

}  // namespace mongo::optimizer::cascades
