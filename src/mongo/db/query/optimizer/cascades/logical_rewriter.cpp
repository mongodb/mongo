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

namespace mongo::optimizer::cascades {

LogicalRewriter::RewriteSet LogicalRewriter::_explorationSet = {
    {LogicalRewriteType::GroupByExplore, 1},
    {LogicalRewriteType::FilterExplore, 1},
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

LogicalRewriter::LogicalRewriter(Memo& memo,
                                 PrefixId& prefixId,
                                 const RewriteSet rewriteSet,
                                 const QueryHints& hints,
                                 const PathToIntervalFn& pathToInterval,
                                 const bool useHeuristicCE)
    : _activeRewriteSet(std::move(rewriteSet)),
      _groupsPending(),
      _memo(memo),
      _prefixId(prefixId),
      _hints(hints),
      _pathToInterval(pathToInterval),
      _useHeuristicCE(useHeuristicCE) {
    initializeRewrites();

    if (_activeRewriteSet.count(LogicalRewriteType::SargableSplit) > 0) {
        // If we are performing SargableSplit exploration rewrite, populate helper map.
        for (const auto& [scanDefName, scanDef] : _memo.getMetadata()._scanDefs) {
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
    NodeIdSet insertNodeIds;

    Memo::NodeTargetGroupMap targetGroupMap;
    if (targetGroupId >= 0) {
        targetGroupMap = {{node.ref(), targetGroupId}};
    }

    const GroupIdType resultGroupId = _memo.integrate(node,
                                                      std::move(targetGroupMap),
                                                      insertNodeIds,
                                                      rule,
                                                      addExistingNodeWithNewChild,
                                                      _useHeuristicCE);

    uassert(6624046,
            "Result group is not the same as target group",
            targetGroupId < 0 || targetGroupId == resultGroupId);

    for (const MemoLogicalNodeId& nodeMemoId : insertNodeIds) {
        if (addExistingNodeWithNewChild && nodeMemoId._groupId == targetGroupId) {
            continue;
        }

        for (const auto [type, priority] : _activeRewriteSet) {
            auto& groupQueue = _memo.getGroup(nodeMemoId._groupId)._logicalRewriteQueue;
            groupQueue.push(std::make_unique<LogicalRewriteEntry>(priority, type, nodeMemoId));

            _groupsPending.insert(nodeMemoId._groupId);
        }
    }

    return {resultGroupId, std::move(insertNodeIds)};
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
        return _rewriter._memo.getMetadata();
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
        return getMemo().getGroup(_aboveNodeId._groupId)._logicalProperties;
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

template <class NodeType>
struct DefaultChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->getChild();
    }

    ABT& operator()(ABT& node) const {
        return node.cast<NodeType>()->getChild();
    }
};

template <class NodeType>
struct LeftChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->getLeftChild();
    }

    ABT& operator()(ABT& node) const {
        return node.cast<NodeType>()->getLeftChild();
    }
};

template <class NodeType>
struct RightChildAccessor {
    const ABT& operator()(const ABT& node) const {
        return node.cast<NodeType>()->getRightChild();
    }

    ABT& operator()(ABT& node) const {
        return node.cast<NodeType>()->getRightChild();
    }
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
    for (const std::string& varName : aboveNodeVarNames) {
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
    const IndexPathSet& nonMultiKeyPaths,
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
    ProjectionRenames projectionRenames;
    if (!intersectPartialSchemaReq(mergedReqs, aboveNode.getReqMap(), projectionRenames)) {
        return {};
    }

    const ProjectionName& scanProjName = indexingAvailability.getScanProjection();
    bool hasEmptyInterval =
        simplifyPartialSchemaReqPaths(scanProjName, nonMultiKeyPaths, mergedReqs);
    if (hasEmptyInterval) {
        return createEmptyValueScanNode(ctx);
    }

    if (mergedReqs.size() > LogicalRewriter::kMaxPartialSchemaReqCount) {
        return {};
    }

    const ScanDefinition& scanDef =
        ctx.getMetadata()._scanDefs.at(indexingAvailability.getScanDefName());
    auto candidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                    scanProjName,
                                                    mergedReqs,
                                                    scanDef,
                                                    ctx.getHints()._fastIndexNullHandling,
                                                    hasEmptyInterval);
    if (hasEmptyInterval) {
        return createEmptyValueScanNode(ctx);
    }

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
                                                scanDef.getNonMultiKeyPathSet(),
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

static void addSargableChildNode(const ABT& node, ABT sargableNode, RewriteContext& ctx) {
    ABT newNode = node;
    newNode.cast<FilterNode>()->getChild() = std::move(sargableNode);
    ctx.addNode(newNode, false /*substitute*/, true /*addExistingNodeWithNewChild*/);
}

template <bool isSubstitution>
static void convertFilterToSargableNode(ABT::reference_type node,
                                        const FilterNode& filterNode,
                                        RewriteContext& ctx) {
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
    for (auto it = conversion->_reqMap.cbegin(); it != conversion->_reqMap.cend();) {
        uassert(6624111,
                "Filter partial schema requirement must contain a variable name.",
                !it->first._projectionName.empty());
        uassert(6624112,
                "Filter partial schema requirement cannot bind.",
                !it->second.hasBoundProjectionName());
        if (isIntervalReqFullyOpenDNF(it->second.getIntervals())) {
            it = conversion->_reqMap.erase(it);
        } else {
            ++it;
        }
    }

    // If the filter has no constraints after removing no-ops, then rewrite the filter with a
    // predicate using the constant True.
    if (conversion->_reqMap.empty()) {
        ctx.addNode(make<FilterNode>(Constant::boolean(true), filterNode.getChild()),
                    isSubstitution);
        return;
    }

    // If in substitution mode, disallow retaining original predicate. If in exploration mode, only
    // allow retaining the original predicate and if we have at least one index available.
    if constexpr (isSubstitution) {
        if (conversion->_retainPredicate) {
            return;
        }
    } else if (!conversion->_retainPredicate || scanDef.getIndexDefs().empty()) {
        return;
    }

    const ProjectionName& scanProjName = indexingAvailability.getScanProjection();
    bool hasEmptyInterval = simplifyPartialSchemaReqPaths(
        scanProjName, scanDef.getNonMultiKeyPathSet(), conversion->_reqMap);
    if (hasEmptyInterval) {
        addEmptyValueScanNode(ctx);
        return;
    }
    if (conversion->_reqMap.size() > LogicalRewriter::kMaxPartialSchemaReqCount) {
        // Too many requirements.
        return;
    }

    auto candidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                    scanProjName,
                                                    conversion->_reqMap,
                                                    scanDef,
                                                    ctx.getHints()._fastIndexNullHandling,
                                                    hasEmptyInterval);

    if (hasEmptyInterval) {
        addEmptyValueScanNode(ctx);
        return;
    }

    auto scanParams = computeScanParams(ctx.getPrefixId(), conversion->_reqMap, scanProjName);
    ABT sargableNode = make<SargableNode>(std::move(conversion->_reqMap),
                                          std::move(candidateIndexes),
                                          std::move(scanParams),
                                          IndexReqTarget::Complete,
                                          filterNode.getChild());
    if (!conversion->_retainPredicate) {
        ctx.addNode(sargableNode, isSubstitution);
        return;
    }

    const GroupIdType childGroupId =
        filterNode.getChild().cast<MemoLogicalDelegatorNode>()->getGroupId();
    if (childGroupId == indexingAvailability.getScanGroupId()) {
        // Add node directly.
        addSargableChildNode(node, std::move(sargableNode), ctx);
    } else {
        // Look at the child group to find SargableNodes and attempt to combine.
        const auto& logicalNodes = ctx.getMemo().getGroup(childGroupId)._logicalNodes;
        for (const ABT& childNode : logicalNodes.getVector()) {
            if (auto childSargableNode = childNode.cast<SargableNode>();
                childSargableNode != nullptr) {
                const auto& result = mergeSargableNodes(indexingAvailability,
                                                        scanDef.getNonMultiKeyPathSet(),
                                                        *sargableNode.cast<SargableNode>(),
                                                        *childSargableNode,
                                                        ctx);
                if (result) {
                    addSargableChildNode(node, std::move(*result), ctx);
                }
            }
        }
    }
}

static ABT appendFieldPath(const FieldPathType& fieldPath, ABT input) {
    for (size_t index = fieldPath.size(); index-- > 0;) {
        input = make<PathGet>(fieldPath.at(index), std::move(input));
    }
    return input;
}

template <>
struct SubstituteConvert<FilterNode> {
    void operator()(ABT::reference_type node, RewriteContext& ctx) {
        const FilterNode& filterNode = *node.cast<FilterNode>();

        // Sub-rewrite: attempt to de-compose filter. If we have a path with a prefix of PathGet's
        // followed by a PathComposeM, then split into two filter nodes at the composition and
        // retain the prefix for each.
        // TODO: consider using a standalone rewrite.
        if (auto evalFilter = filterNode.getFilter().cast<EvalFilter>(); evalFilter != nullptr) {
            ABT::reference_type pathRef = evalFilter->getPath().ref();
            FieldPathType fieldPath;
            for (;;) {
                if (auto newPath = pathRef.cast<PathGet>(); newPath != nullptr) {
                    fieldPath.push_back(newPath->name());
                    pathRef = newPath->getPath().ref();
                } else {
                    break;
                }
            }

            if (auto composition = pathRef.cast<PathComposeM>(); composition != nullptr) {
                // Remove the path composition and insert two filter nodes.
                ABT filterNode1 = make<FilterNode>(
                    make<EvalFilter>(appendFieldPath(fieldPath, composition->getPath1()),
                                     evalFilter->getInput()),
                    filterNode.getChild());
                ABT filterNode2 = make<FilterNode>(
                    make<EvalFilter>(appendFieldPath(fieldPath, composition->getPath2()),
                                     evalFilter->getInput()),
                    std::move(filterNode1));

                ctx.addNode(filterNode2, true /*substitute*/);
                return;
            }
        }

        convertFilterToSargableNode<true /*isSubstitution*/>(node, filterNode, ctx);
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
                    pathKeepPtr->getNames().size() < LogicalRewriter::kMaxPartialSchemaReqCount) {
                    // Optimization. If we are retaining fields on the root level, generate
                    // EvalNodes with the intention of converting later to a SargableNode after
                    // reordering, in order to be able to cover the fields using a physical scan or
                    // index.

                    ABT result = evalNode.getChild();
                    ABT keepPath = make<PathIdentity>();

                    std::set<std::string> orderedSet;
                    for (const std::string& fieldName : pathKeepPtr->getNames()) {
                        orderedSet.insert(fieldName);
                    }
                    for (const std::string& fieldName : orderedSet) {
                        ProjectionName projName = ctx.getPrefixId().getNextId("fieldProj");
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
        if (conversion->_reqMap.size() != 1) {
            // For evaluation nodes we expect to create a single entry.
            return;
        }

        for (auto& [key, req] : conversion->_reqMap) {
            req.setBoundProjectionName(evalNode.getProjectionName());

            uassert(6624114,
                    "Eval partial schema requirement must contain a variable name.",
                    !key._projectionName.empty());
            uassert(6624115,
                    "Eval partial schema requirement cannot have a range",
                    isIntervalReqFullyOpenDNF(req.getIntervals()));
        }

        bool hasEmptyInterval = false;
        auto candidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                        scanProjName,
                                                        conversion->_reqMap,
                                                        scanDef,
                                                        ctx.getHints()._fastIndexNullHandling,
                                                        hasEmptyInterval);

        if (hasEmptyInterval) {
            addEmptyValueScanNode(ctx);
            return;
        }

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
    ABT n = node.getChild();
    const auto reqMap = node.getReqMap();
    for (const auto& [key, req] : reqMap) {
        lowerPartialSchemaRequirement(key, req, n, ctx.getPathToInterval());
    }
    ctx.addNode(n, true /*clear*/);
}

template <class Type>
struct ExploreConvert {
    void operator()(ABT::reference_type nodeRef, RewriteContext& ctx) = delete;
};

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
            !ctx.getMemo().getGroup(scanGroupId)._logicalNodes.at(0).is<ScanNode>()) {
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
        if (collectVariableReferences(node) != VariableNameSetType{scanProjectionName}) {
            // Rewrite not applicable if we refer projections other than the scan projection.
            return;
        }

        const bool isIndex = target == IndexReqTarget::Index;

        const auto& indexFieldPrefixMap = ctx.getIndexFieldPrefixMap();
        const auto indexFieldPrefixMapIt =
            isIndex ? indexFieldPrefixMap.cend() : indexFieldPrefixMap.find(scanDefName);
        const bool indexFieldMapHasScanDef = indexFieldPrefixMapIt != indexFieldPrefixMap.cend();

        const auto& reqMap = sargableNode.getReqMap();

        const bool fastIndexNullHandling = ctx.getHints()._fastIndexNullHandling;
        std::vector<bool> isFullyOpen;
        std::vector<bool> maybeHasNullAndBinds;
        {
            // Pre-compute if a requirement's interval is fully open.
            isFullyOpen.reserve(reqMap.size());
            for (const auto& [key, req] : reqMap) {
                isFullyOpen.push_back(isIntervalReqFullyOpenDNF(req.getIntervals()));
            }

            if (!fastIndexNullHandling && !isIndex) {
                // Pre-compute if needed if a requirement's interval may contain nulls, and also has
                // an output binding.
                maybeHasNullAndBinds.reserve(reqMap.size());
                for (const auto& [key, req] : reqMap) {
                    maybeHasNullAndBinds.push_back(req.hasBoundProjectionName() &&
                                                   checkMaybeHasNull(req.getIntervals()));
                }
            }
        }

        // We iterate over the possible ways to split N predicates into 2^N subsets, one goes to the
        // left, and the other to the right side.
        const size_t reqSize = reqMap.size();
        const size_t highMask = isIndex ? (1ull << (reqSize - 1)) : (1ull << reqSize);
        for (size_t mask = 1; mask < highMask; mask++) {
            PartialSchemaRequirements leftReqs;
            PartialSchemaRequirements rightReqs;
            bool hasFieldCoverage = true;
            bool hasLeftIntervals = false;
            bool hasRightIntervals = false;

            size_t index = 0;
            for (const auto& [key, req] : reqMap) {
                const bool fullyOpenInterval = isFullyOpen.at(index);

                if (((1ull << index) & mask) != 0) {
                    bool addedToLeft = false;
                    if (fastIndexNullHandling || isIndex) {
                        leftReqs.emplace(key, req);
                        addedToLeft = true;
                    } else if (maybeHasNullAndBinds.at(index)) {
                        // We cannot return index values if our interval can possibly contain Null.
                        // Instead, we remove the output binding for the left side, and return the
                        // value from the right (seek) side.
                        if (!fullyOpenInterval) {
                            leftReqs.emplace(key, PartialSchemaRequirement{"", req.getIntervals()});
                            addedToLeft = true;
                        }
                        rightReqs.emplace(
                            key,
                            PartialSchemaRequirement{req.getBoundProjectionName(),
                                                     IntervalReqExpr::makeSingularDNF()});
                    } else {
                        leftReqs.emplace(key, req);
                        addedToLeft = true;
                    }

                    if (addedToLeft) {
                        if (!fullyOpenInterval) {
                            hasLeftIntervals = true;
                        }

                        if (indexFieldMapHasScanDef) {
                            if (auto pathPtr = key._path.cast<PathGet>(); pathPtr != nullptr &&
                                indexFieldPrefixMapIt->second.count(pathPtr->name()) == 0) {
                                // We have found a left requirement which cannot be covered with an
                                // index.
                                hasFieldCoverage = false;
                                break;
                            }
                        }
                    }
                } else {
                    rightReqs.emplace(key, req);

                    if (!fullyOpenInterval) {
                        hasRightIntervals = true;
                    }
                }
                index++;
            }

            if (leftReqs.empty()) {
                // Can happen if we have intervals containing null.
                invariant(!fastIndexNullHandling && !isIndex);
                continue;
            }

            if (isIndex && (!hasLeftIntervals || !hasRightIntervals)) {
                // Reject. Must have at least one proper interval on either side.
                continue;
            }
            if (!hasFieldCoverage) {
                // Reject rewrite. No suitable indexes.
                continue;
            }

            bool hasEmptyLeftInterval = false;
            auto leftCandidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                                scanProjectionName,
                                                                leftReqs,
                                                                scanDef,
                                                                fastIndexNullHandling,
                                                                hasEmptyLeftInterval);
            if (isIndex && leftCandidateIndexes.empty()) {
                // Reject rewrite.
                continue;
            }

            bool hasEmptyRightInterval = false;
            auto rightCandidateIndexes = computeCandidateIndexes(ctx.getPrefixId(),
                                                                 scanProjectionName,
                                                                 rightReqs,
                                                                 scanDef,
                                                                 fastIndexNullHandling,
                                                                 hasEmptyRightInterval);
            if (isIndex && rightCandidateIndexes.empty()) {
                // With empty candidate map, reject only if we cannot implement as Seek.
                continue;
            }
            uassert(6624116,
                    "Empty intervals should already be rewritten to empty ValueScan nodes",
                    !hasEmptyLeftInterval && !hasEmptyRightInterval);

            ABT scanDelegator = make<MemoLogicalDelegatorNode>(scanGroupId);
            ABT leftChild = make<SargableNode>(std::move(leftReqs),
                                               std::move(leftCandidateIndexes),
                                               boost::none,
                                               IndexReqTarget::Index,
                                               scanDelegator);

            auto rightScanParams =
                computeScanParams(ctx.getPrefixId(), rightReqs, scanProjectionName);
            ABT rightChild = rightReqs.empty()
                ? scanDelegator
                : make<SargableNode>(std::move(rightReqs),
                                     std::move(rightCandidateIndexes),
                                     std::move(rightScanParams),
                                     isIndex ? IndexReqTarget::Index : IndexReqTarget::Seek,
                                     scanDelegator);

            ABT newRoot = make<RIDIntersectNode>(scanProjectionName,
                                                 hasLeftIntervals,
                                                 hasRightIntervals,
                                                 std::move(leftChild),
                                                 std::move(rightChild));

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
struct ExploreConvert<FilterNode> {
    void operator()(ABT::reference_type node, RewriteContext& ctx) {
        convertFilterToSargableNode<false /*isSubstitution*/>(node, *node.cast<FilterNode>(), ctx);
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
    if (node.hasLeftIntervals() && hasLeftRef) {
        defaultReorder<AboveNode,
                       RIDIntersectNode,
                       DefaultChildAccessor,
                       LeftChildAccessor,
                       false /*substitute*/>(aboveNode, belowNode, ctx);
    }
    if (node.hasRightIntervals() && hasRightRef) {
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

    registerRewrite(LogicalRewriteType::FilterExplore,
                    &LogicalRewriter::bindSingleNode<FilterNode, ExploreConvert>);
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
        if (_memo.getDebugInfo().exceedsIterationLimit(iterationCount)) {
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
    auto& queue = _memo.getGroup(groupId)._logicalRewriteQueue;
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

        for (size_t i = 0; i < _memo.getGroup(targetGroupId)._logicalNodes.size(); i++) {
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
