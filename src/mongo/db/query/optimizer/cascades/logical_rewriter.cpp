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

#include "mongo/db/query/optimizer/cascades/logical_rewriter.h"

#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/db/query/optimizer/utils/reftracker_utils.h"

namespace mongo::optimizer::cascades {

LogicalRewriter::RewriteSet LogicalRewriter::_explorationSet = {
    {LogicalRewriteType::GroupByExplore, 1},
    {LogicalRewriteType::SargableSplit, 2},
    {LogicalRewriteType::FilterRIDIntersectReorder, 2},
    {LogicalRewriteType::EvaluationRIDIntersectReorder, 2}};

LogicalRewriter::RewriteSet LogicalRewriter::_substitutionSet = {
    {LogicalRewriteType::FilterEvaluationReorder, 1},
    {LogicalRewriteType::FilterCollationReorder, 1},
    {LogicalRewriteType::EvaluationCollationReorder, 1},
    {LogicalRewriteType::EvaluationLimitSkipReorder, 1},

    {LogicalRewriteType::FilterGroupByReorder, 1},
    {LogicalRewriteType::GroupCollationReorder, 1},

    {LogicalRewriteType::FilterUnwindReorder, 1},
    {LogicalRewriteType::EvaluationUnwindReorder, 1},
    {LogicalRewriteType::UnwindCollationReorder, 1},

    {LogicalRewriteType::FilterExchangeReorder, 1},
    {LogicalRewriteType::ExchangeEvaluationReorder, 1},

    {LogicalRewriteType::FilterUnionReorder, 1},

    {LogicalRewriteType::CollationMerge, 1},
    {LogicalRewriteType::LimitSkipMerge, 1},

    {LogicalRewriteType::SargableFilterReorder, 1},
    {LogicalRewriteType::SargableEvaluationReorder, 1},

    {LogicalRewriteType::FilterValueScanPropagate, 1},
    {LogicalRewriteType::EvaluationValueScanPropagate, 1},
    {LogicalRewriteType::SargableValueScanPropagate, 1},
    {LogicalRewriteType::CollationValueScanPropagate, 1},
    {LogicalRewriteType::LimitSkipValueScanPropagate, 1},
    {LogicalRewriteType::ExchangeValueScanPropagate, 1},

    {LogicalRewriteType::LimitSkipSubstitute, 1},

    {LogicalRewriteType::FilterSubstitute, 2},
    {LogicalRewriteType::EvaluationSubstitute, 2},
    {LogicalRewriteType::SargableMerge, 2}};

LogicalRewriter::LogicalRewriter(const Metadata& metadata,
                                 Memo& memo,
                                 PrefixId& prefixId,
                                 const RewriteSet rewriteSet,
                                 const DebugInfo& debugInfo,
                                 const QueryHints& hints,
                                 const PathToIntervalFn& pathToInterval,
                                 const ConstFoldFn& constFold,
                                 const LogicalPropsInterface& logicalPropsDerivation,
                                 const CardinalityEstimator& cardinalityEstimator)
    : _activeRewriteSet(std::move(rewriteSet)),
      _groupsPending(),
      _metadata(metadata),
      _memo(memo),
      _prefixId(prefixId),
      _debugInfo(debugInfo),
      _hints(hints),
      _pathToInterval(pathToInterval),
      _constFold(constFold),
      _logicalPropsDerivation(logicalPropsDerivation),
      _cardinalityEstimator(cardinalityEstimator) {
    initializeRewrites();

    if (_activeRewriteSet.count(LogicalRewriteType::SargableSplit) > 0) {
        // If we are performing SargableSplit exploration rewrite, populate helper map.
        for (const auto& [scanDefName, scanDef] : _metadata._scanDefs) {
            for (const auto& [indexDefName, indexDef] : scanDef.getIndexDefs()) {
                for (const IndexCollationEntry& entry : indexDef.getCollationSpec()) {
                    if (auto pathPtr = entry._path.cast<PathGet>(); pathPtr != nullptr) {
                        _indexFieldPrefixMap[scanDefName].insert(pathPtr->name());
                    }
                }
            }
        }
    }
}

GroupIdType LogicalRewriter::addRootNode(const ABT& node) {
    return addNode(node, -1, LogicalRewriteType::Root, false /*addExistingNodeWithNewChild*/).first;
}

std::pair<GroupIdType, NodeIdSet> LogicalRewriter::addNode(const ABT& node,
                                                           const GroupIdType targetGroupId,
                                                           const LogicalRewriteType rule,
                                                           const bool addExistingNodeWithNewChild) {
    NodeIdSet insertedNodeIds;

    Memo::NodeTargetGroupMap targetGroupMap;
    if (targetGroupId >= 0) {
        targetGroupMap = {{node.ref(), targetGroupId}};
    }

    const GroupIdType resultGroupId = _memo.integrate(
        Memo::Context{&_metadata, &_debugInfo, &_logicalPropsDerivation, &_cardinalityEstimator},
        node,
        std::move(targetGroupMap),
        insertedNodeIds,
        rule);

    uassert(6624046,
            "Result group is not the same as target group",
            targetGroupId < 0 || targetGroupId == resultGroupId);

    // Every memo group that was extended with a new node may have new rewrites that can apply to
    // it, so enqueue each of these groups to be visited by a rewrite later.
    for (const MemoLogicalNodeId& nodeMemoId : insertedNodeIds) {
        // However, if 'addExistingNodeWithNewChild' then don't schedule the 'targetGroupId' for new
        // rewrites, to avoid applying the same rewrite forever.
        if (addExistingNodeWithNewChild && nodeMemoId._groupId == targetGroupId) {
            continue;
        }

        for (const auto [type, priority] : _activeRewriteSet) {
            auto& groupQueue = _memo.getLogicalRewriteQueue(nodeMemoId._groupId);
            groupQueue.push(std::make_unique<LogicalRewriteEntry>(priority, type, nodeMemoId));

            _groupsPending.insert(nodeMemoId._groupId);
        }
    }

    return {resultGroupId, std::move(insertedNodeIds)};
}

void LogicalRewriter::clearGroup(const GroupIdType groupId) {
    _memo.clearLogicalNodes(groupId);
}

class RewriteContext {
public:
    RewriteContext(LogicalRewriter& rewriter,
                   const LogicalRewriteType rule,
                   const MemoLogicalNodeId aboveNodeId,
                   const MemoLogicalNodeId belowNodeId)
        : RewriteContext(rewriter, rule, aboveNodeId, true /*hasBelowNodeId*/, belowNodeId){};

    RewriteContext(LogicalRewriter& rewriter,
                   const LogicalRewriteType rule,
                   const MemoLogicalNodeId aboveNodeId)
        : RewriteContext(rewriter, rule, aboveNodeId, false /*hasBelowNodeId*/, {}){};

    std::pair<GroupIdType, NodeIdSet> addNode(const ABT& node,
                                              const bool substitute,
                                              const bool addExistingNodeWithNewChild = false) {
        if (substitute) {
            uassert(6624110, "Cannot substitute twice", !_hasSubstituted);
            _hasSubstituted = true;

            _rewriter.clearGroup(_aboveNodeId._groupId);
            if (_hasBelowNodeId) {
                _rewriter.clearGroup(_belowNodeId._groupId);
            }
        }
        return _rewriter.addNode(node, _aboveNodeId._groupId, _rule, addExistingNodeWithNewChild);
    }

    Memo& getMemo() const {
        return _rewriter._memo;
    }

    const Metadata& getMetadata() const {
        return _rewriter._metadata;
    }

    PrefixId& getPrefixId() const {
        return _rewriter._prefixId;
    }

    const QueryHints& getHints() const {
        return _rewriter._hints;
    }

    auto& getIndexFieldPrefixMap() const {
        return _rewriter._indexFieldPrefixMap;
    }

    const properties::LogicalProps& getAboveLogicalProps() const {
        return getMemo().getLogicalProps(_aboveNodeId._groupId);
    }

    bool hasSubstituted() const {
        return _hasSubstituted;
    }

    MemoLogicalNodeId getAboveNodeId() const {
        return _aboveNodeId;
    }

    auto& getSargableSplitCountMap() const {
        return _rewriter._sargableSplitCountMap;
    }

    const auto& getPathToInterval() const {
        return _rewriter._pathToInterval;
    }

    const auto& getConstFold() const {
        return _rewriter._constFold;
    }

private:
    RewriteContext(LogicalRewriter& rewriter,
                   const LogicalRewriteType rule,
                   const MemoLogicalNodeId aboveNodeId,
                   const bool hasBelowNodeId,
                   const MemoLogicalNodeId belowNodeId)
        : _aboveNodeId(aboveNodeId),
          _hasBelowNodeId(hasBelowNodeId),
          _belowNodeId(belowNodeId),
          _rewriter(rewriter),
          _hasSubstituted(false),
          _rule(rule){};

    const MemoLogicalNodeId _aboveNodeId;
    const bool _hasBelowNodeId;
    const MemoLogicalNodeId _belowNodeId;

    // We don't own this.
    LogicalRewriter& _rewriter;

    bool _hasSubstituted;

    const LogicalRewriteType _rule;
};

struct ReorderDependencies {
    bool _hasNodeRef = false;
    bool _hasChildRef = false;
    bool _hasNodeAndChildRef = false;
};

template <class AboveType,
          class BelowType,
          template <class> class BelowChildAccessor = DefaultChildAccessor>
ReorderDependencies computeDependencies(ABT::reference_type aboveNodeRef,
                                        ABT::reference_type belowNodeRef,
                                        RewriteContext& ctx) {
    // Get variables from above node and check if they are bound at below node, or at below node's
    // child.
    const auto aboveNodeVarNames = collectVariableReferences(aboveNodeRef);

    ABT belowNode = belowNodeRef;
    VariableEnvironment env = VariableEnvironment::build(belowNode, &ctx.getMemo());
    const DefinitionsMap belowNodeDefs = env.hasDefinitions(belowNode.ref())
        ? env.getDefinitions(belowNode.ref())
        : DefinitionsMap{};
    ABT::reference_type belowChild = BelowChildAccessor<BelowType>()(belowNode).ref();
    const DefinitionsMap belowChildNodeDefs =
        env.hasDefinitions(belowChild) ? env.getDefinitions(belowChild) : DefinitionsMap{};

    ReorderDependencies dependencies;
    for (const ProjectionName& varName : aboveNodeVarNames) {
        auto it = belowNodeDefs.find(varName);
        // Variable is exclusively defined in the below node.
        const bool refersToNode = it != belowNodeDefs.cend() && it->second.definedBy == belowNode;
        // Variable is defined in the belowNode's child subtree.
        const bool refersToChild = belowChildNodeDefs.find(varName) != belowChildNodeDefs.cend();

        if (refersToNode) {
            if (refersToChild) {
                dependencies._hasNodeAndChildRef = true;
            } else {
                dependencies._hasNodeRef = true;
            }
        } else if (refersToChild) {
            dependencies._hasChildRef = true;
        } else {
            // Lambda variable. Ignore.
        }
    }

    return dependencies;
}

static ABT createEmptyValueScanNode(const RewriteContext& ctx) {
    using namespace properties;

    const ProjectionNameSet& projNameSet =
        getPropertyConst<ProjectionAvailability>(ctx.getAboveLogicalProps()).getProjections();
    ProjectionNameVector projNameVector;
    projNameVector.insert(projNameVector.begin(), projNameSet.cbegin(), projNameSet.cend());
    return make<ValueScanNode>(std::move(projNameVector), ctx.getAboveLogicalProps());
}

static void addEmptyValueScanNode(RewriteContext& ctx) {
    ABT newNode = createEmptyValueScanNode(ctx);
    ctx.addNode(newNode, true /*substitute*/);
}

static void defaultPropagateEmptyValueScanNode(const ABT& n, RewriteContext& ctx) {
    if (n.cast<ValueScanNode>()->getArraySize() == 0) {
        addEmptyValueScanNode(ctx);
    }
}

template <class AboveType,
          class BelowType,
          template <class> class AboveChildAccessor = DefaultChildAccessor,
          template <class> class BelowChildAccessor = DefaultChildAccessor,
          bool substitute = true>
void defaultReorder(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) {
    ABT newParent = belowNode;
    ABT newChild = aboveNode;

    std::swap(BelowChildAccessor<BelowType>()(newParent),
              AboveChildAccessor<AboveType>()(newChild));
    BelowChildAccessor<BelowType>()(newParent) = std::move(newChild);

    ctx.addNode(newParent, substitute);
}

template <class AboveType, class BelowType>
void defaultReorderWithDependenceCheck(ABT::reference_type aboveNode,
                                       ABT::reference_type belowNode,
                                       RewriteContext& ctx) {
    const ReorderDependencies dependencies =
        computeDependencies<AboveType, BelowType>(aboveNode, belowNode, ctx);
    if (dependencies._hasNodeRef) {
        // Above node refers to a variable bound by below node.
        return;
    }

    defaultReorder<AboveType, BelowType>(aboveNode, belowNode, ctx);
}

template <class AboveType, class BelowType>
struct SubstituteReorder {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultReorderWithDependenceCheck<AboveType, BelowType>(aboveNode, belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<FilterNode, FilterNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultReorder<FilterNode, FilterNode>(aboveNode, belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<FilterNode, UnionNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        ABT newParent = belowNode;

        for (auto& childOfChild : newParent.cast<UnionNode>()->nodes()) {
            ABT aboveCopy = aboveNode;
            std::swap(aboveCopy.cast<FilterNode>()->getChild(), childOfChild);
            std::swap(childOfChild, aboveCopy);
        }

        ctx.addNode(newParent, true /*substitute*/);
    }
};

template <class AboveType>
void unwindBelowReorder(ABT::reference_type aboveNode,
                        ABT::reference_type unwindNode,
                        RewriteContext& ctx) {
    const ReorderDependencies dependencies =
        computeDependencies<AboveType, UnwindNode>(aboveNode, unwindNode, ctx);
    if (dependencies._hasNodeRef || dependencies._hasNodeAndChildRef) {
        // Above node refers to projection being unwound. Reject rewrite.
        return;
    }

    defaultReorder<AboveType, UnwindNode>(aboveNode, unwindNode, ctx);
}

template <>
struct SubstituteReorder<FilterNode, UnwindNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        unwindBelowReorder<FilterNode>(aboveNode, belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<EvaluationNode, UnwindNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        unwindBelowReorder<EvaluationNode>(aboveNode, belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<UnwindNode, CollationNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        const ProjectionNameSet& collationProjections =
            belowNode.cast<CollationNode>()->getProperty().getAffectedProjectionNames();
        if (collationProjections.find(aboveNode.cast<UnwindNode>()->getProjectionName()) !=
            collationProjections.cend()) {
            // A projection being affected by the collation is being unwound. Reject rewrite.
            return;
        }

        defaultReorder<UnwindNode, CollationNode>(aboveNode, belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<FilterNode, ValueScanNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultPropagateEmptyValueScanNode(belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<EvaluationNode, ValueScanNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultPropagateEmptyValueScanNode(belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<SargableNode, ValueScanNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultPropagateEmptyValueScanNode(belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<CollationNode, ValueScanNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultPropagateEmptyValueScanNode(belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<LimitSkipNode, ValueScanNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultPropagateEmptyValueScanNode(belowNode, ctx);
    }
};

template <>
struct SubstituteReorder<ExchangeNode, ValueScanNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        defaultPropagateEmptyValueScanNode(belowNode, ctx);
    }
};

template <class AboveType, class BelowType>
struct SubstituteMerge {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) = delete;
};

template <>
struct SubstituteMerge<CollationNode, CollationNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        ABT newRoot = aboveNode;
        // Retain above property.
        newRoot.cast<CollationNode>()->getChild() = belowNode.cast<CollationNode>()->getChild();

        ctx.addNode(newRoot, true /*substitute*/);
    }
};

template <>
struct SubstituteMerge<LimitSkipNode, LimitSkipNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        using namespace properties;

        ABT newRoot = aboveNode;
        LimitSkipNode& aboveCollationNode = *newRoot.cast<LimitSkipNode>();
        const LimitSkipNode& belowCollationNode = *belowNode.cast<LimitSkipNode>();

        aboveCollationNode.getChild() = belowCollationNode.getChild();
        combineLimitSkipProperties(aboveCollationNode.getProperty(),
                                   belowCollationNode.getProperty());

        ctx.addNode(newRoot, true /*substitute*/);
    }
};

static boost::optional<ABT> mergeSargableNodes(
    const properties::IndexingAvailability& indexingAvailability,
    const MultikeynessTrie& multikeynessTrie,
    const SargableNode& aboveNode,
    const SargableNode& belowNode,
    RewriteContext& ctx) {
    if (indexingAvailability.getScanGroupId() !=
        belowNode.getChild().cast<MemoLogicalDelegatorNode>()->getGroupId()) {
        // Do not merge if child is not another Sargable node, or the child's child is not a
        // ScanNode.
        return {};
    }

    PartialSchemaRequirements mergedReqs = belowNode.getReqMap();
    if (!intersectPartialSchemaReq(mergedReqs, aboveNode.getReqMap())) {
        return {};
    }

    const ProjectionName& scanProjName = indexingAvailability.getScanProjection();
    ProjectionRenames projectionRenames;
    const bool hasEmptyInterval = simplifyPartialSchemaReqPaths(
        scanProjName, multikeynessTrie, mergedReqs, projectionRenames, ctx.getConstFold());
    if (hasEmptyInterval) {
        return createEmptyValueScanNode(ctx);
    }

    if (PSRExpr::numLeaves(mergedReqs.getRoot()) > SargableNode::kMaxPartialSchemaReqs) {
        return {};
    }

    const ScanDefinition& scanDef =
        ctx.getMetadata()._scanDefs.at(indexingAvailability.getScanDefName());
    auto candidateIndexes = computeCandidateIndexes(
        ctx.getPrefixId(), scanProjName, mergedReqs, scanDef, ctx.getHints(), ctx.getConstFold());

    auto scanParams = computeScanParams(ctx.getPrefixId(), mergedReqs, scanProjName);
    ABT result = make<SargableNode>(std::move(mergedReqs),
                                    std::move(candidateIndexes),
                                    std::move(scanParams),
                                    IndexReqTarget::Complete,
                                    belowNode.getChild());
    applyProjectionRenames(std::move(projectionRenames), result);
    return result;
}

template <>
struct SubstituteMerge<SargableNode, SargableNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        using namespace properties;

        const LogicalProps& props = ctx.getAboveLogicalProps();
        tassert(6624170,
                "At this point we should have IndexingAvailability",
                hasProperty<IndexingAvailability>(props));

        const auto& indexingAvailability = getPropertyConst<IndexingAvailability>(props);
        const ScanDefinition& scanDef =
            ctx.getMetadata()._scanDefs.at(indexingAvailability.getScanDefName());
        tassert(6624171, "At this point the collection must exist", scanDef.exists());

        const auto& result = mergeSargableNodes(indexingAvailability,
                                                scanDef.getMultikeynessTrie(),
                                                *aboveNode.cast<SargableNode>(),
                                                *belowNode.cast<SargableNode>(),
                                                ctx);
        if (result) {
            ctx.addNode(*result, true /*substitute*/);
        }
    }
};

template <class Type>
struct SubstituteConvert {
    void operator()(ABT::reference_type nodeRef, RewriteContext& ctx) = delete;
};

template <>
struct SubstituteConvert<LimitSkipNode> {
    void operator()(ABT::reference_type node, RewriteContext& ctx) {
        if (node.cast<LimitSkipNode>()->getProperty().getLimit() == 0) {
            addEmptyValueScanNode(ctx);
        }
    }
};

static void convertFilterToSargableNode(ABT::reference_type node,
                                        const FilterNode& filterNode,
                                        RewriteContext& ctx,
                                        const ProjectionName& scanProjName) {
    using namespace properties;

    const LogicalProps& props = ctx.getAboveLogicalProps();
    if (!hasProperty<IndexingAvailability>(props)) {
        // Can only convert to sargable node if we have indexing availability.
        return;
    }
    const auto& indexingAvailability = getPropertyConst<IndexingAvailability>(props);
    const ScanDefinition& scanDef =
        ctx.getMetadata()._scanDefs.at(indexingAvailability.getScanDefName());
    if (!scanDef.exists()) {
        // Do not attempt to optimize for non-existing collections.
        return;
    }

    auto conversion = convertExprToPartialSchemaReq(
        filterNode.getFilter(), true /*isFilterContext*/, ctx.getPathToInterval());
    if (!conversion) {
        return;
    }

    // Remove any partial schema requirements which do not constrain their input.

    conversion->_reqMap.simplify([](const PartialSchemaKey& key, PartialSchemaRequirement& req) {
        uassert(6624111,
                "Filter partial schema requirement must contain a variable name.",
                key._projectionName);
        uassert(6624112,
                "Filter partial schema requirement cannot bind.",
                !req.getBoundProjectionName());
        return true;
    });

    ProjectionRenames projectionRenames_unused;
    const bool hasEmptyInterval = simplifyPartialSchemaReqPaths(scanProjName,
                                                                scanDef.getMultikeynessTrie(),
                                                                conversion->_reqMap,
                                                                projectionRenames_unused,
                                                                ctx.getConstFold());
    tassert(6624156,
            "We should not be seeing renames from a converted Filter",
            projectionRenames_unused.empty());

    if (hasEmptyInterval) {
        addEmptyValueScanNode(ctx);
        return;
    }
    if (PSRExpr::numLeaves(conversion->_reqMap.getRoot()) > SargableNode::kMaxPartialSchemaReqs) {
        // Too many requirements.
        return;
    }
    if (conversion->_reqMap.isNoop()) {
        // If the filter has no constraints after removing no-ops, then replace with its child. We
        // need to copy the child since we hold it by reference from the memo, and during
        // subtitution the current group will be erased.

        ABT newNode = filterNode.getChild();
        ctx.addNode(newNode, true /*substitute*/);
        return;
    }

    auto candidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                    scanProjName,
                                                    conversion->_reqMap,
                                                    scanDef,
                                                    ctx.getHints(),
                                                    ctx.getConstFold());

    auto scanParams = computeScanParams(ctx.getPrefixId(), conversion->_reqMap, scanProjName);
    ABT sargableNode = make<SargableNode>(std::move(conversion->_reqMap),
                                          std::move(candidateIndexes),
                                          std::move(scanParams),
                                          IndexReqTarget::Complete,
                                          filterNode.getChild());
    if (conversion->_retainPredicate) {
        // '_retainPredicate' means the 'sargableNode' is an over-approximation, so we also have to
        // keep the original Filter node. But this means the Filter-to-Sargable rewrite could apply
        // again, to avoid rewriting endlessly we need to avoid scheduling this rewrite. So we pass
        // 'addExistingNodeWithNewChild = true'.
        ABT newNode = node;
        newNode.cast<FilterNode>()->getChild() = std::move(sargableNode);
        ctx.addNode(newNode, true /*substitute*/, true /*addExistingNodeWithNewChild*/);
    } else {
        ctx.addNode(sargableNode, true /*substitute*/);
    }
}

/**
 * Takes an expression or path and attempts to remove Not nodes by pushing them
 * down toward the leaves. We only remove a Not if we can combine it into a
 * PathCompare, or cancel it out with another Not.
 *
 * Caller provides:
 * - an input ABT
 * - 'negate': true if we want the new ABT to be the negation of the input.
 *
 * Callee can reply with either:
 * - boost::none, meaning we can't make the ABT any simpler.
 * - struct Simplified, which means we can make the ABT simpler.
 *     - 'newNode' is the replacement.
 *     - 'negated' says whether 'newNode' is the negation of the original.
 *       For example, we can simplify the child of Traverse but not push
 *       a Not through it.
 */
class NotPushdown {
public:
    struct Simplified {
        // True if 'newNode' is the negation of the original node.
        bool negated;
        ABT newNode;
    };
    using Result = boost::optional<Simplified>;

    Result operator()(const ABT& /*n*/, const PathGet& get, const bool negate) {
        if (auto simplified = get.getPath().visit(*this, negate)) {
            return {
                {simplified->negated, make<PathGet>(get.name(), std::move(simplified->newNode))}};
        }
        return {};
    }

    Result operator()(const ABT& /*n*/, const PathCompare& comp, const bool negate) {
        if (!negate) {
            // No rewrite necessary.
            return {};
        }

        if (auto op = negateComparisonOp(comp.op())) {
            return {{true, make<PathCompare>(*op, comp.getVal())}};
        }
        return {};
    }

    Result operator()(const ABT& /*n*/, const UnaryOp& unary, const bool negate) {
        // Only handle Not.
        if (unary.op() != Operations::Not) {
            return {};
        }

        const bool negateChild = !negate;
        if (auto simplified = unary.getChild().visit(*this, negateChild)) {
            // Remove the 'Not' if either:
            // - it can cancel with a Not in some ancestor ('negate')
            // - it can cancel with a Not in the child ('simplified->negated')
            // The 'either' is exclusive because the child is only 'negated' if we
            // requested it ('negateChild').
            const bool removeNot = negate || simplified->negated;
            if (removeNot) {
                // We cancelled with a Not in some ancestor iff the caller asked us to.
                simplified->negated = negate;
            } else {
                simplified->newNode =
                    make<UnaryOp>(Operations::Not, std::move(simplified->newNode));
            }
            return simplified;
        } else {
            // We failed to simplify the child.
            if (negate) {
                // But we can still simplify 'n' by unwrapping the 'Not'.
                return {{true, unary.getChild()}};
            } else {
                // Therefore we failed to simplify 'n'.
                return {};
            }
        }
    }

    Result operator()(const ABT& /*n*/, const PathLambda& pathLambda, const bool negate) {
        const LambdaAbstraction* lambda = pathLambda.getLambda().cast<LambdaAbstraction>();
        if (!lambda) {
            // Shouldn't happen; just don't simplify.
            return {};
        }

        // Try to simplify the lambda body.
        // If that succeeds, it may expose 'PathLambda Lambda [x] EvalFilter p (Variable [x])',
        // which we can simplify to just 'p'. That's only valid if the Variable [x] is the
        // only occurrence of 'x'.

        if (auto simplified = lambda->getBody().visit(*this, negate)) {
            auto&& [negated, newBody] = std::move(*simplified);
            // If the lambda var is used only once, simplifying the body may have exposed
            // 'PathLambda Lambda [x] EvalFilter p (Variable [x])', which we can replace
            // with just 'p'.
            if (auto iter = _varCounts.find(lambda->varName());
                iter != _varCounts.end() && iter->second == 1) {
                if (EvalFilter* evalF = newBody.cast<EvalFilter>()) {
                    if (Variable* input = evalF->getInput().cast<Variable>();
                        input && input->name() == lambda->varName()) {
                        return {{negated, std::exchange(evalF->getPath(), make<Blackhole>())}};
                    }
                }
            }
            return {{
                negated,
                make<PathLambda>(make<LambdaAbstraction>(lambda->varName(), std::move(newBody))),
            }};
        }
        return {};
    }

    Result operator()(const ABT& /*n*/, const PathTraverse& traverse, bool /*negate*/) {
        // We actually don't care whether the caller is asking us to negate.
        // We can't negate a Traverse; the best we can do is simplify the child.
        if (auto simplified = traverse.getPath().visit(*this, false /*negate*/)) {
            tassert(7022400,
                    "NotPushdown unexpectedly negated when asked only to simplify",
                    !simplified->negated);
            simplified->newNode =
                make<PathTraverse>(traverse.getMaxDepth(), std::move(simplified->newNode));
            return simplified;
        } else {
            return {};
        }
    }

    Result operator()(const ABT& /*n*/, const PathComposeM& compose, const bool negate) {
        auto simplified1 = compose.getPath1().visit(*this, negate);
        auto simplified2 = compose.getPath2().visit(*this, negate);
        if (!simplified1 && !simplified2) {
            // Neither child is simplified.
            return {};
        }
        // At least one child is simplified, so we're going to rebuild a node.
        // If either child was not simplified, we're going to copy the original
        // unsimplified child.
        if (!simplified1) {
            simplified1 = {{false, compose.getPath1()}};
        }
        if (!simplified2) {
            simplified2 = {{false, compose.getPath2()}};
        }

        if (!simplified1->negated && !simplified2->negated) {
            // Neither is negated: keep the ComposeM.
            return {{false,
                     make<PathComposeM>(std::move(simplified1->newNode),
                                        std::move(simplified2->newNode))}};
        }
        // At least one child is negated, so we're going to rewrite to ComposeA.
        // If either child was not able to aborb a Not, we'll add an explicit Not to its root.
        if (!simplified1->negated) {
            simplified1 = {{true, negatePath(std::move(simplified1->newNode))}};
        }
        if (!simplified2->negated) {
            simplified2 = {{true, negatePath(std::move(simplified2->newNode))}};
        }
        return {
            {true,
             make<PathComposeA>(std::move(simplified1->newNode), std::move(simplified2->newNode))}};
    }

    Result operator()(const ABT& /*n*/, const EvalFilter& evalF, const bool negate) {
        if (auto simplified = evalF.getPath().visit(*this, negate)) {
            simplified->newNode =
                make<EvalFilter>(std::move(simplified->newNode), evalF.getInput());
            return simplified;
        }
        return {};
    }

    template <typename T>
    Result operator()(const ABT& /*n*/, const T& /*nodeSubclass*/, bool /*negate*/) {
        // We don't know how to simplify this node.
        return {};
    }

    static boost::optional<ABT> simplify(const ABT& n, PrefixId& prefixId) {
        ProjectionNameMap<size_t> varCounts;
        VariableEnvironment::walkVariables(n,
                                           [&](const Variable& var) { ++varCounts[var.name()]; });


        NotPushdown instance{varCounts, prefixId};
        if (auto simplified = n.visit(instance, false /*negate*/)) {
            auto&& [negated, newNode] = std::move(*simplified);
            tassert(7022401,
                    "NotPushdown unexpectedly negated when asked only to simplify",
                    !simplified->negated);
            return newNode;
        }
        return {};
    }

private:
    NotPushdown(const ProjectionNameMap<size_t>& varCounts, PrefixId& prefixId)
        : _varCounts(varCounts), _prefixId(prefixId) {}

    // Take a Path and negate it.  Use Lambda / EvalFilter to toggle between expressions and paths.
    ABT negatePath(ABT path) {
        ProjectionName freshVar = _prefixId.getNextId("tmp_bool");
        return make<PathLambda>(make<LambdaAbstraction>(
            freshVar,
            make<UnaryOp>(Operations::Not,
                          make<EvalFilter>(std::move(path), make<Variable>(freshVar)))));
    }

    const ProjectionNameMap<size_t>& _varCounts;
    PrefixId& _prefixId;
};

/**
 * Attempt to remove Traverse nodes from a FilterNode.
 *
 * If we succeed, add a replacement node to the RewriteContext and return true.
 */
static bool simplifyFilterPath(const FilterNode& filterNode,
                               RewriteContext& ctx,
                               const ProjectionName& scanProjName,
                               const MultikeynessTrie& trie) {
    // Expect the filter to be EvalFilter, or UnaryOp [Not] EvalFilter.
    const ABT& filter = filterNode.getFilter();
    const bool toplevelNot =
        filter.is<UnaryOp>() && filter.cast<UnaryOp>()->op() == Operations::Not;
    const ABT& argument = toplevelNot ? filter.cast<UnaryOp>()->getChild() : filter;
    if (const auto* evalFilter = argument.cast<EvalFilter>()) {
        if (const auto* variable = evalFilter->getInput().cast<Variable>()) {
            // If EvalFilter is applied to the whole-document binding then
            // we can simplify the path using what we know about the multikeyness
            // of the collection.
            if (variable->name() != scanProjName) {
                return false;
            }

            ABT path = evalFilter->getPath();
            if (simplifyTraverseNonArray(path, trie)) {
                ABT newPredicate = make<EvalFilter>(std::move(path), evalFilter->getInput());
                if (toplevelNot) {
                    newPredicate = make<UnaryOp>(Operations::Not, std::move(newPredicate));
                }
                ctx.addNode(make<FilterNode>(std::move(newPredicate), filterNode.getChild()),
                            true /*substitute*/);
                return true;
            }
        }
    }

    return false;
}

template <>
struct SubstituteConvert<FilterNode> {
    void operator()(ABT::reference_type node, RewriteContext& ctx) {
        const FilterNode& filterNode = *node.cast<FilterNode>();

        // Sub-rewrite: attempt to de-compose filter into at least two new filter nodes.
        if (auto* evalFilter = filterNode.getFilter().cast<EvalFilter>()) {
            if (auto result = decomposeToFilterNodes(filterNode.getChild(),
                                                     evalFilter->getPath(),
                                                     evalFilter->getInput(),
                                                     2 /*minDepth*/)) {
                ctx.addNode(*result, true /*substitute*/);
                return;
            }
        }


        using namespace properties;
        const LogicalProps& props = ctx.getAboveLogicalProps();
        if (!hasProperty<IndexingAvailability>(props)) {
            return;
        }
        const auto& indexingAvailability = getPropertyConst<IndexingAvailability>(props);
        const ProjectionName& scanProjName = indexingAvailability.getScanProjection();

        const ScanDefinition& scanDef =
            ctx.getMetadata()._scanDefs.at(indexingAvailability.getScanDefName());
        if (!scanDef.exists()) {
            return;
        }
        const MultikeynessTrie& trie = scanDef.getMultikeynessTrie();

        if (simplifyFilterPath(filterNode, ctx, scanProjName, trie)) {
            return;
        }

        if (auto filter = NotPushdown::simplify(filterNode.getFilter(), ctx.getPrefixId())) {
            ctx.addNode(make<FilterNode>(std::move(*filter), filterNode.getChild()),
                        true /*substitute*/);
            return;
        }

        convertFilterToSargableNode(node, filterNode, ctx, scanProjName);
    }
};

template <>
struct SubstituteConvert<EvaluationNode> {
    void operator()(ABT::reference_type node, RewriteContext& ctx) {
        using namespace properties;

        const LogicalProps props = ctx.getAboveLogicalProps();
        if (!hasProperty<IndexingAvailability>(props)) {
            // Can only convert to sargable node if we have indexing availability.
            return;
        }

        const auto& indexingAvailability = getPropertyConst<IndexingAvailability>(props);
        const ProjectionName& scanProjName = indexingAvailability.getScanProjection();

        const ScanDefinition& scanDef =
            ctx.getMetadata()._scanDefs.at(indexingAvailability.getScanDefName());
        if (!scanDef.exists()) {
            // Do not attempt to optimize for non-existing collections.
            return;
        }

        const EvaluationNode& evalNode = *node.cast<EvaluationNode>();

        // Sub-rewrite: attempt to convert Keep to a chain of individual evaluations.
        // TODO: consider using a standalone rewrite.
        if (auto evalPathPtr = evalNode.getProjection().cast<EvalPath>(); evalPathPtr != nullptr) {
            if (auto inputPtr = evalPathPtr->getInput().cast<Variable>();
                inputPtr != nullptr && inputPtr->name() == scanProjName) {
                if (auto pathKeepPtr = evalPathPtr->getPath().cast<PathKeep>();
                    pathKeepPtr != nullptr &&
                    pathKeepPtr->getNames().size() < SargableNode::kMaxPartialSchemaReqs) {
                    // Optimization. If we are retaining fields on the root level, generate
                    // EvalNodes with the intention of converting later to a SargableNode after
                    // reordering, in order to be able to cover the fields using a physical scan or
                    // index.

                    ABT result = evalNode.getChild();
                    ABT keepPath = make<PathIdentity>();

                    FieldNameOrderedSet orderedSet;
                    for (const FieldNameType& fieldName : pathKeepPtr->getNames()) {
                        orderedSet.insert(fieldName);
                    }
                    for (const FieldNameType& fieldName : orderedSet) {
                        ProjectionName projName{ctx.getPrefixId().getNextId("fieldProj")};
                        result = make<EvaluationNode>(
                            projName,
                            make<EvalPath>(make<PathGet>(fieldName, make<PathIdentity>()),
                                           evalPathPtr->getInput()),
                            std::move(result));

                        maybeComposePath(keepPath,
                                         make<PathField>(fieldName,
                                                         make<PathConstant>(
                                                             make<Variable>(std::move(projName)))));
                    }

                    result = make<EvaluationNode>(
                        evalNode.getProjectionName(),
                        make<EvalPath>(std::move(keepPath), Constant::emptyObject()),
                        std::move(result));
                    ctx.addNode(result, true /*substitute*/);
                    return;
                }
            }
        }

        // We still want to extract sargable nodes from EvalNode to use for PhysicalScans.
        auto conversion = convertExprToPartialSchemaReq(
            evalNode.getProjection(), false /*isFilterContext*/, ctx.getPathToInterval());
        if (!conversion) {
            return;
        }
        uassert(6624165,
                "Should not be getting retainPredicate set for EvalNodes",
                !conversion->_retainPredicate);
        if (PSRExpr::numLeaves(conversion->_reqMap.getRoot()) != 1) {
            // For evaluation nodes we expect to create a single entry.
            return;
        }

        PSRExpr::visitDNF(conversion->_reqMap.getRoot(), [&](PartialSchemaEntry& entry) {
            auto& [key, req] = entry;
            req = {
                evalNode.getProjectionName(), std::move(req.getIntervals()), req.getIsPerfOnly()};

            uassert(6624114,
                    "Eval partial schema requirement must contain a variable name.",
                    key._projectionName);
            uassert(6624115,
                    "Eval partial schema requirement cannot have a range",
                    isIntervalReqFullyOpenDNF(req.getIntervals()));
        });

        ProjectionRenames projectionRenames_unused;
        const bool hasEmptyInterval = simplifyPartialSchemaReqPaths(scanProjName,
                                                                    scanDef.getMultikeynessTrie(),
                                                                    conversion->_reqMap,
                                                                    projectionRenames_unused,
                                                                    ctx.getConstFold());
        if (hasEmptyInterval) {
            addEmptyValueScanNode(ctx);
            return;
        }

        auto candidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                        scanProjName,
                                                        conversion->_reqMap,
                                                        scanDef,
                                                        ctx.getHints(),
                                                        ctx.getConstFold());

        auto scanParams = computeScanParams(ctx.getPrefixId(), conversion->_reqMap, scanProjName);
        ABT newNode = make<SargableNode>(std::move(conversion->_reqMap),
                                         std::move(candidateIndexes),
                                         std::move(scanParams),
                                         IndexReqTarget::Complete,
                                         evalNode.getChild());
        ctx.addNode(newNode, true /*substitute*/);
    }
};

static void lowerSargableNode(const SargableNode& node, RewriteContext& ctx) {
    PhysPlanBuilder builder{node.getChild()};
    const auto& reqMap = node.getReqMap();
    lowerPartialSchemaRequirements(boost::none /*scanGroupCE*/,
                                   boost::none /*baseCE*/,
                                   {} /*indexPredSels*/,
                                   createResidualReqsWithEmptyCE(reqMap.getRoot()),
                                   ctx.getPathToInterval(),
                                   builder);
    ctx.addNode(builder._node, true /*substitute*/);
}

template <class Type>
struct ExploreConvert {
    void operator()(ABT::reference_type nodeRef, RewriteContext& ctx) = delete;
};

/**
 * Used to pre-compute properties of a PSR.
 */
struct RequirementProps {
    bool _mayReturnNull;
};

/**
 * Holds result of splitting requirements into left and right sides to support index+fetch and index
 * intersection.
 */
struct SplitRequirementsResult {
    PSRExprBuilder _leftReqsBuilder;
    PSRExprBuilder _rightReqsBuilder;

    bool _hasFieldCoverage = true;
};

/**
 * Helper transport for 'splitRequirementsFetch': adds a PSRExpr::Node to a builder. The caller can
 * specify whether to keep only predicates, only projections, or both. Implicitly it handles
 * perfOnly predicates: either dropping them (on the fetch side) or converting them to non-perfOnly
 * (on the index side).
 */
struct SplitRequirementsFetchTransport {
    enum class Keep {
        kBoth,
        kPredicateOnly,
        kProjectionOnly,
    };
    static void addReq(const bool left,
                       const PSRExpr::Node& expr,
                       const Keep keep,
                       const boost::optional<FieldNameSet>& indexFieldPrefixMap,
                       PSRExprBuilder& leftReqs,
                       PSRExprBuilder& rightReqs,
                       bool& hasFieldCoverage) {
        auto& builder = left ? leftReqs : rightReqs;

        SplitRequirementsFetchTransport impl{
            left,
            keep,
            indexFieldPrefixMap,
            builder,
            hasFieldCoverage,
        };
        algebra::transport<false>(expr, impl);
    }

    void prepare(const PSRExpr::Conjunction&) {
        builder.pushConj();
    }
    void transport(const PSRExpr::Conjunction&, const PSRExpr::NodeVector&) {
        builder.pop();
    }
    void prepare(const PSRExpr::Disjunction&) {
        builder.pushDisj();
    }
    void transport(const PSRExpr::Disjunction&, const PSRExpr::NodeVector&) {
        builder.pop();
    }
    void transport(const PSRExpr::Atom& node) {
        const bool keepPred = keep != Keep::kProjectionOnly;
        const bool keepProj = keep != Keep::kPredicateOnly;

        const auto& [key, req] = node.getExpr();
        const bool perfOnly = req.getIsPerfOnly();
        const auto outputBinding = keepProj ? req.getBoundProjectionName() : boost::none;
        // perfOnly predicates on the fetch side become trivially true.
        const auto intervals = ((perfOnly && !left) || !keepPred)
            ? IntervalReqExpr::makeSingularDNF()
            : req.getIntervals();

        if (outputBinding || !isIntervalReqFullyOpenDNF(intervals)) {
            builder.atom(key,
                         PartialSchemaRequirement{
                             std::move(outputBinding), std::move(intervals), false /*isPerfOnly*/});

            if (left && indexFieldPrefixMap) {
                if (auto pathPtr = key._path.cast<PathGet>();
                    pathPtr != nullptr && indexFieldPrefixMap->count(pathPtr->name()) == 0) {
                    // We have found a left requirement which cannot be covered with an
                    // index.
                    hasFieldCoverage = false;
                }
            }
        } else {
            // The whole predicate/projection is trivial and its indexability doesn't
            // matter.
        }
    }

    const bool left;
    const Keep keep;
    const boost::optional<FieldNameSet>& indexFieldPrefixMap;

    PSRExprBuilder& builder;
    bool& hasFieldCoverage;
};

/**
 * Takes a vector of PSRExpr, 'conjuncts', and splits them into an index side (on the left) and a
 * fetch side (on the right).
 *
 * The bitfield 'mask' says how to split: each corresponding bit is 1 for left or 0 for right.
 *
 * 'perfOnly' predicates are preserved and converted to non-perfOnly when they go on the index side.
 * On the fetch side they are dropped, by converting them to trivially-true.
 *
 * If yielding-tolerant plans are requested (by 'hints._disableYieldingTolerantPlans == false') then
 * any predicate that should go on the left, we actually put on both sides.
 *
 * Some special cases apply when we attempt to put a predicate on the index side:
 * - If yielding-tolerant plans are requested (by 'hints._disableYieldingTolerantPlans == false')
 *   then we put the predicate on both sides.
 * - If correct null handling is requested (by 'hints._fastIndexNullHandling == false') and the
 *   predicate may contain null, we satisfy its output projection (if any) on the fetch side
 *   instead.
 */
static SplitRequirementsResult splitRequirementsFetch(
    const size_t mask,
    const QueryHints& hints,
    const std::vector<RequirementProps>& reqProps,
    const boost::optional<FieldNameSet>& indexFieldPrefixMap,
    const PSRExpr::NodeVector& conjuncts) {

    bool hasFieldCoverage = true;
    PSRExprBuilder leftReqs;
    PSRExprBuilder rightReqs;
    leftReqs.pushConj();
    rightReqs.pushConj();

    // Adds a PSRExpr 'expr' to the left or right, as specified by 'left'.
    // When adding to the right, replaces any 'perfOnly' atoms with trivially-true.
    // When adding to the left, keeps 'perfOnly' atoms and marks them non-perfOnly.
    //
    // 'keep' specifies whether to keep only the predicate, only the projection, or both.
    // It defaults to both.
    //
    // If we end up adding an unindexed path (one we know does not appear in any index),
    // set 'hasFieldCoverage' to false as a signal to bail out.
    using Keep = SplitRequirementsFetchTransport::Keep;
    const auto addReq =
        [&](const bool left, const PSRExpr::Node& expr, const Keep keep = Keep::kBoth) {
            SplitRequirementsFetchTransport::addReq(
                left, expr, keep, indexFieldPrefixMap, leftReqs, rightReqs, hasFieldCoverage);
        };

    size_t index = 0;

    for (const auto& conjunct : conjuncts) {
        const auto& reqProp = reqProps.at(index);
        const bool left = ((1ull << index) & mask);

        if (!left) {
            // Predicate should go on the right side.
            addReq(false /*left*/, conjunct);
            index++;
            continue;
        }

        // Predicate should go on the left side. However:
        // - Correct null handling requires moving the projection to the fetch side.
        // - Yield-safe plans require duplicating the predicate to both sides.
        //     - Except that 'perfOnly' predicates become true on the fetch side.

        if (hints._fastIndexNullHandling || !reqProp._mayReturnNull) {
            // We can never return Null values from the requirement.
            if (hints._disableYieldingTolerantPlans) {
                // Insert into left side unchanged.
                addReq(true /*left*/, conjunct);

            } else {
                // Insert a requirement on the right side too, left side is non-binding.
                addReq(true /*left*/, conjunct, Keep::kPredicateOnly);
                addReq(false /*left*/, conjunct);
            }
        } else {
            // At this point we should not be seeing perf-only predicates.

            // We cannot return index values, since the interval can possibly contain Null. Instead,
            // we remove the output binding for the left side, and return the value from the
            // right (seek) side.
            addReq(true /*left*/, conjunct, Keep::kPredicateOnly);
            addReq(false /*left*/,
                   conjunct,
                   // Yield-safe plans keep both the predicate and projection on the fetch side.
                   // Yield-unsafe plans only need the projection.
                   hints._disableYieldingTolerantPlans ? Keep::kProjectionOnly : Keep::kBoth);
        }

        if (!hasFieldCoverage) {
            break;
        }
        index++;
    }

    return {
        std::move(leftReqs),
        std::move(rightReqs),
        hasFieldCoverage,
    };
}

static SplitRequirementsResult splitRequirementsIndex(const size_t mask,
                                                      const PSRExpr::NodeVector& reqs,
                                                      const bool disjunctive) {
    PSRExprBuilder leftReqs;
    PSRExprBuilder rightReqs;
    if (disjunctive) {
        leftReqs.pushDisj();
        rightReqs.pushDisj();
    } else {
        leftReqs.pushConj();
        rightReqs.pushConj();
    }

    size_t index = 0;
    for (const auto& req : reqs) {
        if ((1ull << index) & mask) {
            leftReqs.subtree(req);
        } else {
            rightReqs.subtree(req);
        }

        index++;
    }

    return {std::move(leftReqs), std::move(rightReqs)};
}

template <>
struct ExploreConvert<SargableNode> {
    void operator()(ABT::reference_type node, RewriteContext& ctx) {
        using namespace properties;

        const SargableNode& sargableNode = *node.cast<SargableNode>();
        const IndexReqTarget target = sargableNode.getTarget();
        if (target == IndexReqTarget::Seek) {
            return;
        }

        const LogicalProps& props = ctx.getAboveLogicalProps();
        const auto& indexingAvailability = getPropertyConst<IndexingAvailability>(props);
        const GroupIdType scanGroupId = indexingAvailability.getScanGroupId();
        if (sargableNode.getChild().cast<MemoLogicalDelegatorNode>()->getGroupId() != scanGroupId ||
            !ctx.getMemo().getLogicalNodes(scanGroupId).front().is<ScanNode>()) {
            // We are not sitting above a ScanNode.
            lowerSargableNode(sargableNode, ctx);
            return;
        }

        const std::string& scanDefName = indexingAvailability.getScanDefName();
        const ScanDefinition& scanDef = ctx.getMetadata()._scanDefs.at(scanDefName);
        if (scanDef.getIndexDefs().empty()) {
            // Do not insert RIDIntersect if we do not have indexes available.
            return;
        }

        const auto aboveNodeId = ctx.getAboveNodeId();
        auto& sargableSplitCountMap = ctx.getSargableSplitCountMap();
        const size_t splitCount = sargableSplitCountMap[aboveNodeId];
        if (splitCount > LogicalRewriter::kMaxSargableNodeSplitCount) {
            // We cannot split this node further.
            return;
        }

        const ProjectionName& scanProjectionName = indexingAvailability.getScanProjection();
        if (collectVariableReferences(node) != ProjectionNameSet{scanProjectionName}) {
            // Rewrite not applicable if we refer projections other than the scan projection.
            return;
        }

        const bool isIndex = target == IndexReqTarget::Index;

        // Decide whether to do a conjunctive or disjunctive split.
        // Rearrange the predicates so that the top-level node is the one we want to split:
        // - DNF if we want a disjunctive split.
        // - CNF if we want a conjunctive split.
        boost::optional<PSRExpr::Node> splittable;
        {
            const auto& reqMap = sargableNode.getReqMap().getRoot();
            if (isIndex) {
                // When targeting an index, do a disjunctive split if possible.
                if (PSRExpr::isSingletonDisjunction(reqMap)) {
                    // Trivial disjunction means we can only do a conjunctive split.
                    splittable = PSRExpr::convertToCNF(reqMap, SargableNode::kMaxPartialSchemaReqs);
                    tassert(6902602,
                            "converting DNF with only trivial disjunction to CNF should never fail",
                            splittable);
                } else {
                    splittable = reqMap;
                }
            } else {
                // When targeting 'Complete', the only split we allow is index/fetch,
                // because we want to do all union/intersection of record IDs within the index side,
                // to avoid redundant fetching.

                // Index/fetch is a conjunctive split.
                splittable = PSRExpr::convertToCNF(reqMap);
            }
        }
        if (!splittable) {
            // Conversion between DNF/CNF can fail if the result would be too big.
            return;
        }
        const bool disjunctive = splittable->is<PSRExpr::Disjunction>();
        const PSRExpr::NodeVector& reqs = disjunctive
            ? splittable->cast<PSRExpr::Disjunction>()->nodes()
            : splittable->cast<PSRExpr::Conjunction>()->nodes();

        const auto& indexFieldPrefixMap = ctx.getIndexFieldPrefixMap();
        boost::optional<FieldNameSet> indexFieldPrefixMapForScanDef;
        if (auto it = indexFieldPrefixMap.find(scanDefName);
            it != indexFieldPrefixMap.cend() && !isIndex) {
            indexFieldPrefixMapForScanDef = it->second;
        }

        const auto& hints = ctx.getHints();

        // Pre-computed properties of the requirements.
        // We only need these for the index/fetch split.
        std::vector<RequirementProps> reqProps;
        if (!isIndex) {
            reqProps.reserve(reqs.size());
            for (const auto& conjunct : reqs) {
                // Pre-compute if a requirement's interval is fully open.

                // Pre-compute if a requirement's interval may contain nulls, and also has an output
                // binding. Do use constant folding if we do not have to.
                const bool mayReturnNull = !hints._fastIndexNullHandling && !isIndex &&
                    PSRExpr::any(conjunct, [&](const PartialSchemaEntry& entry) {
                        return entry.second.mayReturnNull(ctx.getConstFold());
                    });

                reqProps.push_back({
                    mayReturnNull,
                });
            }
        }

        // We iterate over the possible ways to split N predicates into 2^N subsets, one goes to the
        // left, and the other to the right side. If splitting into Index+Seek (isIndex = false), we
        // try having at least one predicate on the left (mask = 1), and we try all possible
        // subsets. For index intersection however (isIndex = true), we try symmetric partitioning
        // (thus the high bound is 2^(N-1)).
        const size_t highMask = isIndex ? (1ull << (reqs.size() - 1)) : (1ull << reqs.size());
        for (size_t mask = 1; mask < highMask; mask++) {
            auto splitResult = isIndex
                ? splitRequirementsIndex(mask, reqs, disjunctive)
                : splitRequirementsFetch(
                      mask, hints, reqProps, indexFieldPrefixMapForScanDef, reqs);
            if (!splitResult._hasFieldCoverage) {
                // Reject rewrite. No suitable indexes.
                continue;
            }
            auto leftReqExpr = splitResult._leftReqsBuilder.finish();
            auto rightReqExpr = splitResult._rightReqsBuilder.finish();

            if (!leftReqExpr) {
                // Can happen if we have intervals containing null.
                invariant(!hints._fastIndexNullHandling && !isIndex);
                continue;
            }
            // Convert everything back to DNF.
            if (!PSRExpr::isDNF(*leftReqExpr)) {
                leftReqExpr = PSRExpr::convertToDNF(std::move(*leftReqExpr));
                if (!leftReqExpr) {
                    continue;
                }
            }
            if (rightReqExpr && !PSRExpr::isDNF(*rightReqExpr)) {
                rightReqExpr = PSRExpr::convertToDNF(std::move(*rightReqExpr));
                if (!rightReqExpr) {
                    continue;
                }
            }
            boost::optional<PartialSchemaRequirements> leftReqs;
            if (leftReqExpr) {
                leftReqs.emplace(std::move(*leftReqExpr));
            }
            boost::optional<PartialSchemaRequirements> rightReqs;
            if (rightReqExpr) {
                rightReqs.emplace(std::move(*rightReqExpr));
            }

            // DNF / CNF conversions can create redundant predicates; try to simplify.
            // If the reqs are too big, even after simplification, creating a SargableNode will
            // fail, so bail out.
            const auto isTooBig = [&](const PSRExpr::Node& reqs) -> bool {
                return PSRExpr::numLeaves(reqs) > SargableNode::kMaxPartialSchemaReqs;
            };
            if (leftReqs) {
                PartialSchemaRequirements::simplifyRedundantDNF(leftReqs->getRoot());
                ProjectionRenames renames;
                const bool hasEmptyInterval =
                    simplifyPartialSchemaReqPaths(scanProjectionName,
                                                  scanDef.getMultikeynessTrie(),
                                                  *leftReqs,
                                                  renames,
                                                  ctx.getConstFold());
                tassert(6902605,
                        "Did not expect projection renames from CNF -> DNF conversion",
                        renames.empty());
                if (hasEmptyInterval) {
                    continue;
                }
                if (isTooBig(leftReqs->getRoot())) {
                    continue;
                }
            }
            if (rightReqs) {
                PartialSchemaRequirements::simplifyRedundantDNF(rightReqs->getRoot());
                ProjectionRenames renames;
                const bool hasEmptyInterval =
                    simplifyPartialSchemaReqPaths(scanProjectionName,
                                                  scanDef.getMultikeynessTrie(),
                                                  *rightReqs,
                                                  renames,
                                                  ctx.getConstFold());
                tassert(6902604,
                        "Did not expect projection renames from CNF -> DNF conversion",
                        renames.empty());
                if (hasEmptyInterval) {
                    continue;
                }
                if (isTooBig(rightReqs->getRoot())) {
                    continue;
                }
            }

            const bool hasLeftintervals = hasProperIntervals(leftReqs->getRoot());
            const bool hasRightIntervals = rightReqs && hasProperIntervals(rightReqs->getRoot());
            if (isIndex) {
                if (!hasLeftintervals || !hasRightIntervals) {
                    // Reject. Must have at least one proper interval on either side.
                    continue;
                }
            } else if (hints._forceIndexScanForPredicates && hasRightIntervals) {
                // Reject. We must satisfy all intervals via indexes.
                continue;
            }

            auto leftCandidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                                scanProjectionName,
                                                                *leftReqs,
                                                                scanDef,
                                                                hints,
                                                                ctx.getConstFold());
            if (isIndex && leftCandidateIndexes.empty() &&
                PSRExpr::isSingletonDisjunction(leftReqs->getRoot())) {
                // Reject rewrite, because further splitting can only be conjunctive,
                // which does not increase the set of candidate indexes.
                continue;
            }

            CandidateIndexes rightCandidateIndexes;
            if (rightReqs) {
                rightCandidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                                scanProjectionName,
                                                                *rightReqs,
                                                                scanDef,
                                                                hints,
                                                                ctx.getConstFold());
            }

            if (isIndex && rightCandidateIndexes.empty() &&
                PSRExpr::isSingletonDisjunction(rightReqs->getRoot())) {
                // Reject rewrite, because further splitting can only be conjunctive,
                // which does not increase the set of candidate indexes.
                continue;
            }

            ABT scanDelegator = make<MemoLogicalDelegatorNode>(scanGroupId);
            ABT leftChild = make<SargableNode>(std::move(*leftReqs),
                                               std::move(leftCandidateIndexes),
                                               boost::none,
                                               IndexReqTarget::Index,
                                               scanDelegator);

            boost::optional<ScanParams> rightScanParams;
            if (rightReqs) {
                rightScanParams =
                    computeScanParams(ctx.getPrefixId(), *rightReqs, scanProjectionName);
            }

            ABT rightChild = rightReqs
                ? make<SargableNode>(std::move(*rightReqs),
                                     std::move(rightCandidateIndexes),
                                     std::move(rightScanParams),
                                     isIndex ? IndexReqTarget::Index : IndexReqTarget::Seek,
                                     scanDelegator)
                : scanDelegator;

            ABT newRoot = disjunctive
                ? make<RIDUnionNode>(
                      scanProjectionName, std::move(leftChild), std::move(rightChild))
                : make<RIDIntersectNode>(
                      scanProjectionName, std::move(leftChild), std::move(rightChild));

            const auto& result = ctx.addNode(newRoot, false /*substitute*/);
            for (const MemoLogicalNodeId nodeId : result.second) {
                if (!(nodeId == aboveNodeId)) {
                    sargableSplitCountMap[nodeId] = splitCount + 1;
                }
            }
        }
    }
};

template <>
struct ExploreConvert<GroupByNode> {
    void operator()(ABT::reference_type node, RewriteContext& ctx) {
        const GroupByNode& groupByNode = *node.cast<GroupByNode>();
        if (groupByNode.getType() != GroupNodeType::Complete) {
            return;
        }

        ProjectionNameVector preaggVariableNames;
        ABTVector preaggExpressions;

        const ABTVector& aggExpressions = groupByNode.getAggregationExpressions();
        for (const ABT& expr : aggExpressions) {
            const FunctionCall* aggPtr = expr.cast<FunctionCall>();
            if (aggPtr == nullptr) {
                return;
            }

            // In order to be able to pre-aggregate for now we expect a simple aggregate like
            // SUM(x).
            const auto& aggFnName = aggPtr->name();
            if (aggFnName != "$sum" && aggFnName != "$min" && aggFnName != "$max") {
                // TODO: allow more functions.
                return;
            }
            uassert(6624117, "Invalid argument count", aggPtr->nodes().size() == 1);

            preaggVariableNames.push_back(ctx.getPrefixId().getNextId("preagg"));
            preaggExpressions.emplace_back(
                make<FunctionCall>(aggFnName, makeSeq(make<Variable>(preaggVariableNames.back()))));
        }

        ABT localGroupBy = make<GroupByNode>(groupByNode.getGroupByProjectionNames(),
                                             std::move(preaggVariableNames),
                                             aggExpressions,
                                             GroupNodeType::Local,
                                             groupByNode.getChild());

        ABT newRoot = make<GroupByNode>(groupByNode.getGroupByProjectionNames(),
                                        groupByNode.getAggregationProjectionNames(),
                                        std::move(preaggExpressions),
                                        GroupNodeType::Global,
                                        std::move(localGroupBy));

        ctx.addNode(newRoot, false /*substitute*/);
    }
};

template <class AboveType, class BelowType>
struct ExploreReorder {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const = delete;
};

template <class AboveNode>
void reorderAgainstRIDIntersectNode(ABT::reference_type aboveNode,
                                    ABT::reference_type belowNode,
                                    RewriteContext& ctx) {
    const ReorderDependencies leftDeps =
        computeDependencies<AboveNode, RIDIntersectNode, LeftChildAccessor>(
            aboveNode, belowNode, ctx);
    uassert(6624118, "RIDIntersect cannot bind projections", !leftDeps._hasNodeRef);
    const bool hasLeftRef = leftDeps._hasChildRef;

    const ReorderDependencies rightDeps =
        computeDependencies<AboveNode, RIDIntersectNode, RightChildAccessor>(
            aboveNode, belowNode, ctx);
    uassert(6624119, "RIDIntersect cannot bind projections", !rightDeps._hasNodeRef);
    const bool hasRightRef = rightDeps._hasChildRef;

    if (hasLeftRef == hasRightRef) {
        // Both left and right reorderings available means that we refer to both left and right
        // sides.
        return;
    }

    const RIDIntersectNode& node = *belowNode.cast<RIDIntersectNode>();
    const GroupIdType groupIdLeft =
        node.getLeftChild().cast<MemoLogicalDelegatorNode>()->getGroupId();
    const bool hasProperIntervalLeft =
        properties::getPropertyConst<properties::IndexingAvailability>(
            ctx.getMemo().getLogicalProps(groupIdLeft))
            .hasProperInterval();
    if (hasProperIntervalLeft && hasLeftRef) {
        defaultReorder<AboveNode,
                       RIDIntersectNode,
                       DefaultChildAccessor,
                       LeftChildAccessor,
                       false /*substitute*/>(aboveNode, belowNode, ctx);
    }

    const GroupIdType groupIdRight =
        node.getRightChild().cast<MemoLogicalDelegatorNode>()->getGroupId();
    const bool hasProperIntervalRight =
        properties::getPropertyConst<properties::IndexingAvailability>(
            ctx.getMemo().getLogicalProps(groupIdRight))
            .hasProperInterval();
    if (hasProperIntervalRight && hasRightRef) {
        defaultReorder<AboveNode,
                       RIDIntersectNode,
                       DefaultChildAccessor,
                       RightChildAccessor,
                       false /*substitute*/>(aboveNode, belowNode, ctx);
    }
};

template <>
struct ExploreReorder<FilterNode, RIDIntersectNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        reorderAgainstRIDIntersectNode<FilterNode>(aboveNode, belowNode, ctx);
    }
};

template <>
struct ExploreReorder<EvaluationNode, RIDIntersectNode> {
    void operator()(ABT::reference_type aboveNode,
                    ABT::reference_type belowNode,
                    RewriteContext& ctx) const {
        reorderAgainstRIDIntersectNode<EvaluationNode>(aboveNode, belowNode, ctx);
    }
};

void LogicalRewriter::registerRewrite(const LogicalRewriteType rewriteType, RewriteFn fn) {
    if (_activeRewriteSet.find(rewriteType) != _activeRewriteSet.cend()) {
        const bool inserted = _rewriteMap.emplace(rewriteType, fn).second;
        invariant(inserted);
    }
}

void LogicalRewriter::initializeRewrites() {
    registerRewrite(
        LogicalRewriteType::FilterEvaluationReorder,
        &LogicalRewriter::bindAboveBelow<FilterNode, EvaluationNode, SubstituteReorder>);
    registerRewrite(LogicalRewriteType::FilterCollationReorder,
                    &LogicalRewriter::bindAboveBelow<FilterNode, CollationNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::EvaluationCollationReorder,
        &LogicalRewriter::bindAboveBelow<EvaluationNode, CollationNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::EvaluationLimitSkipReorder,
        &LogicalRewriter::bindAboveBelow<EvaluationNode, LimitSkipNode, SubstituteReorder>);
    registerRewrite(LogicalRewriteType::FilterGroupByReorder,
                    &LogicalRewriter::bindAboveBelow<FilterNode, GroupByNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::GroupCollationReorder,
        &LogicalRewriter::bindAboveBelow<GroupByNode, CollationNode, SubstituteReorder>);
    registerRewrite(LogicalRewriteType::FilterUnwindReorder,
                    &LogicalRewriter::bindAboveBelow<FilterNode, UnwindNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::EvaluationUnwindReorder,
        &LogicalRewriter::bindAboveBelow<EvaluationNode, UnwindNode, SubstituteReorder>);
    registerRewrite(LogicalRewriteType::UnwindCollationReorder,
                    &LogicalRewriter::bindAboveBelow<UnwindNode, CollationNode, SubstituteReorder>);

    registerRewrite(LogicalRewriteType::FilterExchangeReorder,
                    &LogicalRewriter::bindAboveBelow<FilterNode, ExchangeNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::ExchangeEvaluationReorder,
        &LogicalRewriter::bindAboveBelow<ExchangeNode, EvaluationNode, SubstituteReorder>);

    registerRewrite(LogicalRewriteType::FilterUnionReorder,
                    &LogicalRewriter::bindAboveBelow<FilterNode, UnionNode, SubstituteReorder>);

    registerRewrite(
        LogicalRewriteType::CollationMerge,
        &LogicalRewriter::bindAboveBelow<CollationNode, CollationNode, SubstituteMerge>);
    registerRewrite(
        LogicalRewriteType::LimitSkipMerge,
        &LogicalRewriter::bindAboveBelow<LimitSkipNode, LimitSkipNode, SubstituteMerge>);

    registerRewrite(LogicalRewriteType::SargableFilterReorder,
                    &LogicalRewriter::bindAboveBelow<SargableNode, FilterNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::SargableEvaluationReorder,
        &LogicalRewriter::bindAboveBelow<SargableNode, EvaluationNode, SubstituteReorder>);

    registerRewrite(LogicalRewriteType::LimitSkipSubstitute,
                    &LogicalRewriter::bindSingleNode<LimitSkipNode, SubstituteConvert>);

    registerRewrite(LogicalRewriteType::SargableMerge,
                    &LogicalRewriter::bindAboveBelow<SargableNode, SargableNode, SubstituteMerge>);
    registerRewrite(LogicalRewriteType::FilterSubstitute,
                    &LogicalRewriter::bindSingleNode<FilterNode, SubstituteConvert>);
    registerRewrite(LogicalRewriteType::EvaluationSubstitute,
                    &LogicalRewriter::bindSingleNode<EvaluationNode, SubstituteConvert>);

    registerRewrite(LogicalRewriteType::FilterValueScanPropagate,
                    &LogicalRewriter::bindAboveBelow<FilterNode, ValueScanNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::EvaluationValueScanPropagate,
        &LogicalRewriter::bindAboveBelow<EvaluationNode, ValueScanNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::SargableValueScanPropagate,
        &LogicalRewriter::bindAboveBelow<SargableNode, ValueScanNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::CollationValueScanPropagate,
        &LogicalRewriter::bindAboveBelow<CollationNode, ValueScanNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::LimitSkipValueScanPropagate,
        &LogicalRewriter::bindAboveBelow<LimitSkipNode, ValueScanNode, SubstituteReorder>);
    registerRewrite(
        LogicalRewriteType::ExchangeValueScanPropagate,
        &LogicalRewriter::bindAboveBelow<ExchangeNode, ValueScanNode, SubstituteReorder>);

    registerRewrite(LogicalRewriteType::GroupByExplore,
                    &LogicalRewriter::bindSingleNode<GroupByNode, ExploreConvert>);
    registerRewrite(LogicalRewriteType::SargableSplit,
                    &LogicalRewriter::bindSingleNode<SargableNode, ExploreConvert>);

    registerRewrite(LogicalRewriteType::FilterRIDIntersectReorder,
                    &LogicalRewriter::bindAboveBelow<FilterNode, RIDIntersectNode, ExploreReorder>);
    registerRewrite(
        LogicalRewriteType::EvaluationRIDIntersectReorder,
        &LogicalRewriter::bindAboveBelow<EvaluationNode, RIDIntersectNode, ExploreReorder>);
}

bool LogicalRewriter::rewriteToFixPoint() {
    int iterationCount = 0;

    while (!_groupsPending.empty()) {
        iterationCount++;
        if (_debugInfo.exceedsIterationLimit(iterationCount)) {
            // Iteration limit exceeded.
            return false;
        }

        const GroupIdType groupId = *_groupsPending.begin();
        rewriteGroup(groupId);
        _groupsPending.erase(groupId);
    }

    return true;
}

void LogicalRewriter::rewriteGroup(const GroupIdType groupId) {
    auto& queue = _memo.getLogicalRewriteQueue(groupId);
    while (!queue.empty()) {
        LogicalRewriteEntry rewriteEntry = std::move(*queue.top());
        // TODO: check if rewriteEntry is different than previous (remove duplicates).
        queue.pop();

        _rewriteMap.at(rewriteEntry._type)(this, rewriteEntry._nodeId, rewriteEntry._type);
    }
}

template <class AboveType, class BelowType, template <class, class> class R>
void LogicalRewriter::bindAboveBelow(const MemoLogicalNodeId nodeMemoId,
                                     const LogicalRewriteType rule) {
    // Get a reference to the node instead of the node itself.
    // Rewrites insert into the memo and can move it.
    ABT::reference_type node = _memo.getNode(nodeMemoId);
    const GroupIdType currentGroupId = nodeMemoId._groupId;

    if (node.is<AboveType>()) {
        // Try to bind as parent.
        const GroupIdType targetGroupId = node.cast<AboveType>()
                                              ->getChild()
                                              .template cast<MemoLogicalDelegatorNode>()
                                              ->getGroupId();

        for (size_t i = 0; i < _memo.getLogicalNodes(targetGroupId).size(); i++) {
            const MemoLogicalNodeId targetNodeId{targetGroupId, i};
            auto targetNode = _memo.getNode(targetNodeId);
            if (targetNode.is<BelowType>()) {
                RewriteContext ctx(*this, rule, nodeMemoId, targetNodeId);
                R<AboveType, BelowType>()(node, targetNode, ctx);
                if (ctx.hasSubstituted()) {
                    return;
                }
            }
        }
    }

    if (node.is<BelowType>()) {
        // Try to bind as child.
        NodeIdSet usageNodeIdSet;
        {
            const auto& inputGroupsToNodeId = _memo.getInputGroupsToNodeIdMap();
            auto it = inputGroupsToNodeId.find({currentGroupId});
            if (it != inputGroupsToNodeId.cend()) {
                usageNodeIdSet = it->second;
            }
        }

        for (const MemoLogicalNodeId& parentNodeId : usageNodeIdSet) {
            auto targetNode = _memo.getNode(parentNodeId);
            if (targetNode.is<AboveType>()) {
                uassert(6624047,
                        "Parent child groupId mismatch (usage map index incorrect?)",
                        targetNode.cast<AboveType>()
                                ->getChild()
                                .template cast<MemoLogicalDelegatorNode>()
                                ->getGroupId() == currentGroupId);

                RewriteContext ctx(*this, rule, parentNodeId, nodeMemoId);
                R<AboveType, BelowType>()(targetNode, node, ctx);
                if (ctx.hasSubstituted()) {
                    return;
                }
            }
        }
    }
}

template <class Type, template <class> class R>
void LogicalRewriter::bindSingleNode(const MemoLogicalNodeId nodeMemoId,
                                     const LogicalRewriteType rule) {
    // Get a reference to the node instead of the node itself.
    // Rewrites insert into the memo and can move it.
    ABT::reference_type node = _memo.getNode(nodeMemoId);
    if (node.is<Type>()) {
        RewriteContext ctx(*this, rule, nodeMemoId);
        R<Type>()(node, ctx);
    }
}

const LogicalRewriter::RewriteSet& LogicalRewriter::getExplorationSet() {
    return _explorationSet;
}

const LogicalRewriter::RewriteSet& LogicalRewriter::getSubstitutionSet() {
    return _substitutionSet;
}

}  // namespace mongo::optimizer::cascades
