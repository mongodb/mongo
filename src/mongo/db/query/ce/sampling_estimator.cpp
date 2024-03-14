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
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/rewrites/sampling_const_eval.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

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

/**
 * Transport to build empty props for each node since the plan is constructed without the memo
 * implementation phase, while the later SBE lowering phase still assumes the props map to be
 * populated (e.g. the FilterNodes lowered from '_residualRequirements' of a SargableNode).
 */
class BuildingPropsTransport {
public:
    BuildingPropsTransport(NodeToGroupPropsMap& propsMap) : _propsMap(propsMap) {}

    template <typename T, typename... Args>
    void transport(ABT& n, const T& node, Args&&... args) {
        _propsMap.emplace(n.cast<Node>(), NodeProps{});
    }

    void buildProps(ABT& n) {
        algebra::transport<true>(n, *this);
    }

private:
    NodeToGroupPropsMap& _propsMap;
};

/**
 * Transport to implement SargableNode into FilterNode, LimitSkipNode and PhysicalScanNode
 * with field projections. Particularly, this transport is interested in the '_residualRequirements'
 * and '_fieldProjectionMap' in the 'ScanParams' of the SargableNode. '_residualRequirements' is
 * lowered into FilterNode(s) and '_fieldProjectionMap' is turned into the field projection map of a
 * PhysicalScanNode.
 */
class ImplementationTransport {
public:
    ImplementationTransport(const Memo& memo,
                            const int64_t sampleSize,
                            const Metadata& metadata,
                            const properties::IndexingAvailability& indexingAvailability,
                            const PathToIntervalFn& pathToIntervalFn,
                            NodeToGroupPropsMap& propsMap)
        : _memo(memo),
          _sampleSize(sampleSize),
          _metadata(metadata),
          _indexingAvailability(indexingAvailability),
          _pathToIntervalFn(pathToIntervalFn),
          _propsMap(propsMap) {}
    void transport(ABT& n,
                   SargableNode& node,
                   ABT& /*childResult*/,
                   ABT& /*bindResult*/,
                   ABT& /*refsResult*/) {
        const auto& scanDefName = _indexingAvailability.getScanDefName();

        // Prepares node properties for the scan node.
        NodeProps props;
        ProjectionNameVector projectionNames;
        for (const auto& entry : node.getScanParams()->_fieldProjectionMap._fieldProjections) {
            projectionNames.push_back(entry.second);
        }
        properties::setPropertyOverwrite<properties::ProjectionRequirement>(
            props._physicalProps, properties::ProjectionRequirement{std::move(projectionNames)});

        // Creates a physical scan node with the field projeciton map from 'node'.
        auto physicalScanNode =
            make<PhysicalScanNode>(std::move(node.getScanParams()->_fieldProjectionMap),
                                   scanDefName,
                                   false /*canUseParallelScan*/,
                                   _metadata._scanDefs.at(scanDefName).getScanOrder());
        _propsMap.emplace(physicalScanNode.cast<Node>(), props);

        // Creates a LimitSkipNode on top of the PhysicalScanNode.
        auto limitSkipNode = make<LimitSkipNode>(properties::LimitSkipRequirement(_sampleSize, 0),
                                                 std::move(physicalScanNode));
        _propsMap.emplace(limitSkipNode.cast<Node>(), std::move(props));


        auto residualReqs = *node.getScanParams()->_residualRequirements;
        PhysPlanBuilder builder;
        std::swap(builder._node, limitSkipNode);

        // Lowers 'residualReqs' and creates FilterNode(s) on top of the ABT 'normalized'.
        lowerPartialSchemaRequirements(boost::none /*scanGroupCE*/,
                                       boost::none /*baseCE*/,
                                       {} /*indexPredSels*/,
                                       createResidualReqsWithEmptyCE(residualReqs),
                                       _pathToIntervalFn,
                                       builder);
        std::swap(n, builder._node);
    }

    /**
     * Template to handle all other cases.
     */
    template <typename T, typename... Args>
    void transport(ABT& n, const T& node, Args&&... args) {
        static_assert(!std::is_same_v<T, SargableNode>, "Missing SargableNode handler");
    }

    void lower(ABT& n) {
        algebra::transport<true>(n, *this);
    }

private:
    const Memo& _memo;
    const int64_t _sampleSize;
    const Metadata& _metadata;
    const properties::IndexingAvailability& _indexingAvailability;
    const PathToIntervalFn& _pathToIntervalFn;
    NodeToGroupPropsMap& _propsMap;
};

/**
 * Helper for drawing a repeatable sample of record IDs from a collection: repeated calls to
 * 'chooseRIDs()' with the same arguments return the same result.
 */
class RIDsCache {
public:
    RIDsCache(const Metadata& metadata, PrefixId& prefixId, const SamplingExecutor& executor)
        : _metadata(metadata), _prefixId(prefixId), _executor(executor) {}

    /**
     * Chooses 'numRids' randomly from the collection named by 'scanDefName',
     * but repeated calls with the same 'numRids' and 'scanDefName' return the same result.
     */
    Constant chooseRIDs(std::string scanDefName, int64_t numRids) {

        std::pair<std::string, int64_t> cacheKey{std::move(scanDefName), numRids};
        if (auto it = _cache.find(cacheKey); it != _cache.end()) {
            return it->second;
        }


        ProjectionName ridProj = _prefixId.getNextId("rid");
        ProjectionName allRidsProj = _prefixId.getNextId("allRids");
        NodeToGroupPropsMap props;
        NodeProps nodeProps{._ridProjName = {ridProj}};

        ABT physicalScan =
            make<PhysicalScanNode>(FieldProjectionMap{._ridProjection = ProjectionName{ridProj}},
                                   cacheKey.first,
                                   false /* useParallelScan */,
                                   ScanOrder::Random);
        props.emplace(physicalScan.cast<Node>(), nodeProps);

        ABT limitNode = make<LimitSkipNode>(properties::LimitSkipRequirement{numRids, 0},
                                            std::move(physicalScan));
        props.emplace(limitNode.cast<Node>(), nodeProps);

        ABT groupNode = make<GroupByNode>(
            // Empty group key: combine all rows into one group.
            ProjectionNameVector{},
            // Output projection holds an array of all the RIDs.
            ProjectionNameVector{allRidsProj},
            makeSeq(make<FunctionCall>("$push", makeSeq(make<Variable>(ridProj)))),
            std::move(limitNode));
        props.emplace(groupNode.cast<Node>(), nodeProps);

        ABT root = make<RootNode>(properties::ProjectionRequirement{{{allRidsProj}}},
                                  std::move(groupNode));
        props.emplace(root.cast<Node>(), nodeProps);

        PlanAndProps planAndProps{std::move(root), std::move(props)};

        auto [tag, val] = _executor.execute(_metadata, {} /*queryParameters*/, planAndProps);
        Constant c{tag, val};
        auto [it, inserted] = _cache.emplace(std::move(cacheKey), std::move(c));
        invariant(inserted);
        invariant(it != _cache.end());

        return it->second;
    }

private:
    const Metadata& _metadata;
    PrefixId& _prefixId;
    const SamplingExecutor& _executor;
    opt::unordered_map<std::pair<std::string, int64_t>, Constant> _cache;
};

class SamplingChunksTransport {
public:
    /**
     * Transport which replaces the 'Limit PhysicalScan' subplan with a more efficient
     * 'NLJ ...' plan, which reduces the amount of seeking by taking chunks of adjacent documents.
     *
     * If 'ridsCache' is non-null, the transport uses it to draw a sample of RIDs only once;
     * the RIDs are baked in to the resulting plan.
     */
    SamplingChunksTransport(NodeToGroupPropsMap& propsMap,
                            const int64_t numChunks,
                            const RIDProjectionsMap& ridProjections,
                            PrefixId& prefixId,
                            RIDsCache* ridsCache)
        : _propsMap(propsMap),
          _numChunks(numChunks),
          _ridProjections(ridProjections),
          _prefixId(prefixId),
          _ridsCache(ridsCache) {}

    void transport(ABT& n, const LimitSkipNode& limit, ABT& child) {
        if (limit.getProperty().getSkip() != 0 || !child.is<PhysicalScanNode>()) {
            return;
        }
        // Extract parts of the input.
        const PhysicalScanNode& physicalScan = *child.cast<PhysicalScanNode>();
        const auto& ridProj = _ridProjections.at(physicalScan.getScanDefName());
        const NodeProps& oldScanProps = _propsMap.at(&physicalScan);
        const NodeProps& oldLimitProps = _propsMap.at(n.cast<Node>());
        const int64_t sampleSize = limit.getProperty().getLimit();

        // Decide how to divide the desired sample size up into chunks.
        const int64_t numChunks = std::min(_numChunks, sampleSize);
        const int64_t chunkSize = sampleSize / numChunks;

        // The rewritten plan has the same logical and physical properties as the original.
        NodeProps resultProps = NodeProps{
            oldLimitProps._planNodeId,
            oldLimitProps._groupId,
            oldLimitProps._logicalProps,
            oldLimitProps._physicalProps,
            boost::none /*ridProjName*/,
            CostType::fromDouble(0),
            CostType::fromDouble(0),
            CEType{1.0} /*adjustedCE*/
        };

        // The outer loop's properties are the same, but with an added RID binding.
        NodeProps outerLoopProps = resultProps;
        properties::getProperty<properties::ProjectionRequirement>(outerLoopProps._physicalProps)
            .getProjections() = ProjectionNameVector{ridProj};

        ABT outerLoop = makeOuterLoop(numChunks,
                                      std::move(outerLoopProps),
                                      ridProj,
                                      physicalScan.getScanDefName(),
                                      physicalScan.useParallelScan(),
                                      physicalScan.getScanOrder());

        // The inner loop produces a chunk of documents for each record ID.
        // Each chunk has 'chunkSize' documents.
        ABT innerLoop =
            ann(resultProps,
                make<LimitSkipNode>(properties::LimitSkipRequirement(chunkSize, 0),
                                    ann(oldScanProps,
                                        make<SeekNode>(ridProj,
                                                       physicalScan.getFieldProjectionMap(),
                                                       physicalScan.getScanDefName()))));

        ABT nlj = ann(resultProps,
                      make<NestedLoopJoinNode>(JoinType::Inner,
                                               ProjectionNameSet{ridProj},
                                               Constant::boolean(true),
                                               std::move(outerLoop),
                                               std::move(innerLoop)));

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
    /**
     * Annotate the node with the given properties, by inserting to _propsMap.
     */
    ABT ann(NodeProps props, ABT node) {
        Node* n = node.cast<Node>();
        tassert(8375703, "Expected a Node", n);
        _propsMap.emplace(n, std::move(props));
        return node;
    }

    ABT makeOuterLoop(int32_t numChunks,
                      NodeProps props,
                      ProjectionName ridProj,
                      std::string scanDefName,
                      bool useParallelScan,
                      ScanOrder scanOrder) {
        if (_ridsCache) {
            Constant rids = _ridsCache->chooseRIDs(std::move(scanDefName), numChunks);

            // Properties the Evaluation child, which provides / requires no projections.
            NodeProps noProj{props};
            getProperty<properties::ProjectionRequirement>(noProj._physicalProps)
                .getProjections() = {};

            return ann(props,
                       make<UnwindNode>(
                           ridProj,
                           _prefixId.getNextId("unusedUnwindIndex"),
                           false /*retainNonArrays*/,
                           ann(props,
                               make<EvaluationNode>(
                                   ridProj,
                                   make<Constant>(std::move(rids)),
                                   ann(noProj,
                                       make<LimitSkipNode>(properties::LimitSkipRequirement(1, 0),
                                                           ann(noProj, make<CoScanNode>())))))));
        } else {
            return ann(props,
                       make<LimitSkipNode>(
                           properties::LimitSkipRequirement(numChunks, 0),
                           ann(props,
                               make<PhysicalScanNode>(
                                   FieldProjectionMap{._ridProjection = ProjectionName{ridProj}},
                                   scanDefName,
                                   useParallelScan,
                                   scanOrder))));
        }
    }

    NodeToGroupPropsMap& _propsMap;
    const int64_t _numChunks;
    const RIDProjectionsMap& _ridProjections;
    PrefixId& _prefixId;
    RIDsCache* _ridsCache = nullptr;
};

class SamplingTransport {

public:
    SamplingTransport(OptPhaseManager phaseManager,
                      const int64_t numRecords,
                      DebugInfo debugInfo,
                      PrefixId& prefixId,
                      std::unique_ptr<cascades::CardinalityEstimator> fallbackCE,
                      std::unique_ptr<SamplingExecutor> executor)
        : _phaseManager(std::move(phaseManager)),
          _sampleSize(std::min<int64_t>(
              _phaseManager.getHints()._sqrtSampleSizeEnabled ? std::sqrt(numRecords) : numRecords,
              _phaseManager.getHints()._samplingCollectionSizeMax)),
          _debugInfo(std::move(debugInfo)),
          _prefixId(prefixId),
          _fallbackCE(std::move(fallbackCE)),
          _executor(std::move(executor)),
          _ridsCache(_phaseManager.getMetadata(), prefixId, *_executor) {}

    CERecord transport(const ABT::reference_type n,
                       const FilterNode& node,
                       const Metadata& metadata,
                       const cascades::Memo& memo,
                       const properties::LogicalProps& logicalProps,
                       const QueryParameterMap& queryParameters,
                       CERecord childResult,
                       CERecord /*exprResult*/) {
        if (_phaseManager.getHints()._forceSamplingCEFallBackForFilterNode ||
            !properties::hasProperty<properties::IndexingAvailability>(logicalProps)) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, queryParameters, n);
        }

        SamplingPlanExtractor planExtractor(memo, _phaseManager, _sampleSize);
        // Create a plan with all eval nodes so far and the filter last.
        ABT abtTree = make<FilterNode>(node.getFilter(), planExtractor.extract(n.copy()));

        return estimateFilterCE(
            metadata, memo, logicalProps, queryParameters, n, std::move(abtTree), childResult._ce);
    }

    CERecord transport(const ABT::reference_type n,
                       const SargableNode& node,
                       const Metadata& metadata,
                       const cascades::Memo& memo,
                       const properties::LogicalProps& logicalProps,
                       const QueryParameterMap& queryParameters,
                       CERecord childResult,
                       CERecord /*bindResult*/,
                       CERecord /*refsResult*/) {
        if (!properties::hasProperty<properties::IndexingAvailability>(logicalProps)) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, queryParameters, n);
        }
        const auto& indexingAvailability =
            getPropertyConst<properties::IndexingAvailability>(logicalProps);
        // TODO(SERVER-84713): Remove once a SargableNode always has a ScanNode child after the
        // substitution phase.
        if (node.getChild().cast<MemoLogicalDelegatorNode>()->getGroupId() !=
            indexingAvailability.getScanGroupId()) {
            // To implement a sargable node, we must have the scan group as a child.
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, queryParameters, n);
        }

        const ScanDefinition& scanDef =
            getScanDefFromIndexingAvailability(metadata, indexingAvailability);

        // If there are at least two indexed fields, this ABT pair will hold the paths of the
        // two indexed fields that appear most frequently across all indexes. If such a pair exists,
        // canCombine will be set to true.
        boost::optional<std::pair<ABT, ABT>> paths;
        bool canCombine = false;
        if (_phaseManager.getHints()._sampleTwoFields &&
            PSRExpr::isSingletonDisjunction(node.getReqMap())) {
            const IndexPathOccurrences& indexMap = scanDef.getIndexPathOccurrences();
            std::vector<std::pair<int, ABT>> indexedFields;
            PSRExpr::visitSingletonDNF(
                node.getReqMap(),
                [&](const PartialSchemaEntry& entry, const PSRExpr::VisitorContext&) {
                    const ABT& path = entry.first._path;
                    if (indexMap.contains(path)) {
                        indexedFields.emplace_back(indexMap.at(path), path);
                    }
                });
            if (indexedFields.size() > 1) {
                std::partial_sort(indexedFields.begin(),
                                  indexedFields.begin() + 2,
                                  indexedFields.end(),
                                  [](const auto& a, const auto& b) { return a.first < b.first; });
                paths = std::make_pair(indexedFields.at(0).second, indexedFields.at(1).second);
                canCombine = true;
            }
        }
        // If there exist two suitable fields to estimate together, one will be held in
        // conjKeyPair.second until its match is found by the lambda below. conjKeyPair.first is
        // used to denote its index within the conjunction.
        boost::optional<std::pair<size_t, PartialSchemaEntry>> conjKeyPair;
        std::string estimationMode;

        size_t entryIndex = 0;
        // Estimate individual requirements separately by potentially re-using cached results.
        // TODO: consider estimating together the entire set of requirements (but caching!)
        EstimatePartialSchemaEntrySelFn estimateFn = [&](SelectivityTreeBuilder& selTreeBuilder,
                                                         const PartialSchemaEntry& e) {
            const auto& [key, req] = e;
            if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                // Collects the partial schema entries to rebuild a sargable node. If there exist
                // two suitable fields to sample together, 'conjEntries' will contains 2 entries.
                // Otherwise, it contains only one entry 'e'.
                std::vector<std::pair<size_t, PartialSchemaEntry>> conjEntries;
                conjEntries.push_back({entryIndex, e});
                if (canCombine) {
                    if (conjKeyPair.has_value()) {
                        if ((conjKeyPair->second.first._path == paths->first &&
                             key._path == paths->second) ||
                            (conjKeyPair->second.first._path == paths->second &&
                             key._path == paths->first)) {
                            conjEntries.push_back(*conjKeyPair);
                            conjKeyPair = boost::none;
                        }
                    } else if (key._path == paths->first || key._path == paths->second) {
                        conjKeyPair = std::make_pair(entryIndex, e);
                        return;
                    }
                }
                // Rebuilds a normalized sargable node whose 'reqMap' is reconstructed from
                // 'conjEntries'.
                auto normalized = normalizeSargableNode(node, conjEntries);

                // Continue the sampling estimation only if the field from the partial schema is
                // indexed.
                const bool shouldSample = isFieldPathIndexed(key, scanDef) ||
                    !_phaseManager.getHints()._sampleIndexedFields;
                const CERecord& filterCE = shouldSample
                    ? estimateFilterCE(metadata,
                                       memo,
                                       logicalProps,
                                       queryParameters,
                                       n,
                                       std::move(normalized),
                                       childResult._ce)
                    : _fallbackCE->deriveCE(metadata, memo, logicalProps, queryParameters, n);
                const SelectivityType sel =
                    childResult._ce > 0.0 ? (filterCE._ce / childResult._ce) : SelectivityType{0.0};
                selTreeBuilder.atom(sel);
                estimationMode = filterCE._mode;
            }

            entryIndex++;
        };
        PartialSchemaRequirementsCardinalityEstimator estimator(estimateFn, childResult._ce);
        return {estimator.estimateCE(node.getReqMap()), estimationMode};
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    CERecord transport(ABT::reference_type n,
                       const T& /*node*/,
                       const Metadata& metadata,
                       const cascades::Memo& memo,
                       const properties::LogicalProps& logicalProps,
                       const QueryParameterMap& queryParameters,
                       Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, queryParameters, n);
        }
        return {0.0, samplingLabel};
    }

    CERecord derive(const Metadata& metadata,
                    const cascades::Memo& memo,
                    const properties::LogicalProps& logicalProps,
                    const QueryParameterMap& queryParameters,
                    const ABT::reference_type logicalNodeRef) {
        return algebra::transport<true>(
            logicalNodeRef, *this, metadata, memo, logicalProps, queryParameters);
    }

private:
    CERecord estimateFilterCE(const Metadata& metadata,
                              const cascades::Memo& memo,
                              const properties::LogicalProps& logicalProps,
                              const QueryParameterMap& queryParameters,
                              const ABT::reference_type n,
                              ABT abtTree,
                              CEType childResult) {
        auto it = _selectivityCacheMap.find(abtTree);
        if (it != _selectivityCacheMap.cend()) {
            // Cache hit.
            return {it->second * childResult, samplingLabel};
        }

        const auto selectivity = estimateSelectivity(memo, logicalProps, abtTree);

        _selectivityCacheMap.emplace(std::move(abtTree), selectivity);

        OPTIMIZER_DEBUG_LOG(6264805,
                            5,
                            "CE sampling estimated filter selectivity",
                            "selectivity"_attr = selectivity._value);
        return {selectivity * childResult, samplingLabel};
    }

    optimizer::SelectivityType estimateSelectivity(const cascades::Memo& memo,
                                                   const properties::LogicalProps& logicalProps,
                                                   ABT abt) {
        bool isSargableNode = abt.is<SargableNode>();

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

        PlanAndProps planAndProps = isSargableNode
            ? implementSargableNode(memo, logicalProps, std::move(abt))
            : _phaseManager.optimizeAndReturnProps(std::move(abt));

        // If internalCascadesOptimizerSampleChunks is a positive integer, sample by chunks using
        // that value as the number of chunks. Otherwise, perform fully randomized sample.
        if (const int64_t numChunks = _phaseManager.getHints()._numSamplingChunks; numChunks > 0) {
            SamplingChunksTransport instance{
                planAndProps._map,
                numChunks,
                _phaseManager.getRIDProjections(),
                _prefixId,
                _phaseManager.getHints()._repeatableSample ? &_ridsCache : nullptr,
            };
            algebra::transport<true>(planAndProps._node, instance);

            OPTIMIZER_DEBUG_LOG(6264807,
                                5,
                                "Physical Sampling",
                                "explain"_attr = ExplainGenerator::explainV2(planAndProps._node));
        }

        // (To appease clang, avoid structured bindings here: it apparently doesn't like when
        // they're captured in a lambda, such as the one tasserted() uses.)
        const auto execResult = _executor->execute(
            _phaseManager.getMetadata(), _phaseManager.getQueryParameters(), planAndProps);
        const auto tag = execResult.first;
        const auto value = execResult.second;
        sbe::value::ValueGuard guard{tag, value};

        if (tag == sbe::value::TypeTags::Nothing) {
            // If Group returned 0 results, then nothing passed the filter, so estimate 0.0
            // selectivity.
            return {0.0};
        } else if (tag == sbe::value::TypeTags::NumberInt64) {
            return {static_cast<double>(value) / _sampleSize};
        } else {
            tasserted(8375702,
                      str::stream() << "Sampling executor returned an unexpected type: "
                                    << printTagAndVal(tag, value));
        }
    }

    /**
     * Rebuilds a SargableNode by reconstructing 'reqMap' from 'conjEntries'. 'conjEntries' may
     * contain more than one predicates, for instance, when we are sampling 2 predicates at a time.
     *
     * Also, normalizes the SargableNode with 'getExistingOrTempProjForFieldName()' to ensure the
     * same field names always resolved with the same projection name. This is essential to make two
     * equivalent sampling queries hit the cache '_selectivityCacheMap' even though they are
     * generated from different SargableNode trees in different memo groups.
     *
     * Lastly, omits anything irrelevant for sampling queries such as candidate indexes or output
     * bindings.
     */
    ABT normalizeSargableNode(
        const SargableNode& node,
        const std::vector<std::pair<size_t, PartialSchemaEntry>>& conjEntries) {
        FieldProjectionMap fpm;
        BoolExprBuilder<PartialSchemaEntry> reqs;
        BoolExprBuilder<ResidualRequirement> residualReqs;
        reqs.pushDisj().pushConj();
        residualReqs.pushDisj().pushConj();
        for (const auto& e : conjEntries) {
            auto& [key, req] = e.second;
            auto fieldPath = *key._path.cast<PathGet>();
            auto projectionName =
                getExistingOrTempProjForFieldName(_prefixId, fieldPath.name(), _fieldProjMap);
            fpm._fieldProjections.emplace(fieldPath.name(), projectionName);

            // Strips the output binding from the requirement.
            PartialSchemaRequirement reqWithoutProjection{
                boost::none, req.getIntervals(), req.getIsPerfOnly()};
            reqs.atom(key, reqWithoutProjection);
            residualReqs.atom(
                {{projectionName, fieldPath.getPath()}, reqWithoutProjection, e.first});
        }

        // Notes that we pass empty candidate indexes. This is for better caching in
        // '_selectivityCacheMap' as long as two sargable nodes have the same 'reqMap'.
        return make<SargableNode>(*reqs.finish(),
                                  CandidateIndexes{},
                                  ScanParams{fpm, *residualReqs.finish()},
                                  IndexReqTarget::Complete,
                                  node.getChild());
    }

    /**
     * Implements sargable node without full round-trip.
     */
    PlanAndProps implementSargableNode(const cascades::Memo& memo,
                                       const properties::LogicalProps& logicalProps,
                                       ABT abt) {
        PlanAndProps planAndProps{std::move(abt), NodeToGroupPropsMap{}};
        ImplementationTransport{
            memo,
            _sampleSize,
            _phaseManager.getMetadata(),
            properties::getPropertyConst<properties::IndexingAvailability>(logicalProps),
            _phaseManager.getPathToInterval(),
            planAndProps._map}
            .lower(planAndProps._node);
        BuildingPropsTransport{planAndProps._map}.buildProps(planAndProps._node);

        if (_phaseManager.hasPhase(OptPhase::PathLower)) {
            PathLowering instance{_prefixId};
            int optimizeIterations = 0;
            for (; instance.optimize(planAndProps._node); optimizeIterations++) {
                tassert(
                    8158902,
                    str::stream()
                        << "Iteration limit exceeded while running the following phase: PathLower.",
                    !_debugInfo.exceedsIterationLimit(optimizeIterations));
            }
        }
        if (_phaseManager.hasPhase(OptPhase::ConstEvalPost_ForSampling)) {
            SamplingConstEval{}.optimize(planAndProps._node);
        }
        return planAndProps;
    }

    const ScanDefinition& getScanDefFromIndexingAvailability(
        const Metadata& metadata,
        const properties::IndexingAvailability& indexingAvalability) const {
        auto scanDefIter = metadata._scanDefs.find(indexingAvalability.getScanDefName());
        uassert(8073400,
                "Scan def of indexing avalability is not found",
                scanDefIter != metadata._scanDefs.cend());
        return scanDefIter->second;
    }

    /**
     * Returns true if the field path from the partial schema entry is indexed.
     */
    inline bool isFieldPathIndexed(const PartialSchemaKey& key,
                                   const ScanDefinition& scanDef) const {
        return scanDef.getIndexPathOccurrences().contains(key._path);
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

    const int64_t _sampleSize;
    DebugInfo _debugInfo;
    PrefixId& _prefixId;
    std::unique_ptr<cascades::CardinalityEstimator> _fallbackCE;
    std::unique_ptr<SamplingExecutor> _executor;
    RIDsCache _ridsCache;

    // Reassigns projection names in a sargable node in 'normalizeSargableNode()' in order to
    // normalize the ABT for '_selectivityCacheMap'. This is only used with
    // 'getExistingOrTempProjForFieldName()'.
    FieldProjectionMap _fieldProjMap;

    static constexpr char samplingLabel[] = "sampling";
};

SamplingEstimator::SamplingEstimator(OptPhaseManager phaseManager,
                                     const int64_t numRecords,
                                     DebugInfo debugInfo,
                                     PrefixId& prefixId,
                                     std::unique_ptr<cascades::CardinalityEstimator> fallbackCE,
                                     std::unique_ptr<SamplingExecutor> executor)
    : _transport(std::make_unique<SamplingTransport>(std::move(phaseManager),
                                                     numRecords,
                                                     std::move(debugInfo),
                                                     prefixId,
                                                     std::move(fallbackCE),
                                                     std::move(executor))) {}

SamplingEstimator::~SamplingEstimator() {}

CERecord SamplingEstimator::deriveCE(const Metadata& metadata,
                                     const cascades::Memo& memo,
                                     const properties::LogicalProps& logicalProps,
                                     const QueryParameterMap& queryParameters,
                                     const ABT::reference_type logicalNodeRef) const {
    return _transport->derive(metadata, memo, logicalProps, queryParameters, logicalNodeRef);
}

}  // namespace mongo::optimizer::ce
