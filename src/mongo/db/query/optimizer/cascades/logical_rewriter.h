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

#pragma once

#include <queue>

#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer::cascades {

class LogicalRewriter {
    friend class RewriteContext;

public:
    /**
     * Maximum size of PartialSchemaRequirements for a SargableNode.
     * This limits the number of splits for index intersection.
     */
    static constexpr size_t kMaxPartialSchemaReqCount = 10;

    /*
     * How many times are we allowed to split a sargable node to facilitate index intersection.
     * Results in at most 2^N index intersections.
     */
    static constexpr size_t kMaxSargableNodeSplitCount = 2;

    /**
     * Map of rewrite type to rewrite priority
     */
    using RewriteSet = opt::unordered_map<LogicalRewriteType, double>;

    LogicalRewriter(Memo& memo,
                    PrefixId& prefixId,
                    RewriteSet rewriteSet,
                    const QueryHints& hints,
                    const PathToIntervalFn& pathToInterval,
                    bool useHeuristicCE);

    LogicalRewriter() = delete;
    LogicalRewriter(const LogicalRewriter& other) = delete;
    LogicalRewriter(LogicalRewriter&& other) = default;

    GroupIdType addRootNode(const ABT& node);
    std::pair<GroupIdType, NodeIdSet> addNode(const ABT& node,
                                              GroupIdType targetGroupId,
                                              LogicalRewriteType rule,
                                              bool addExistingNodeWithNewChild);
    void clearGroup(GroupIdType groupId);

    /**
     * Performs logical rewrites across all groups until a fix point is reached.
     * Use this method to perform "standalone" rewrites.
     */
    bool rewriteToFixPoint();

    /**
     * Performs rewrites only for a particular group. Use this method to perform rewrites driven by
     * top-down optimization.
     */
    void rewriteGroup(GroupIdType groupId);

    static const RewriteSet& getExplorationSet();
    static const RewriteSet& getSubstitutionSet();

private:
    using RewriteFn = std::function<void(
        LogicalRewriter* rewriter, const MemoLogicalNodeId nodeId, const LogicalRewriteType rule)>;
    using RewriteFnMap = opt::unordered_map<LogicalRewriteType, RewriteFn>;

    /**
     * Attempts to perform a reordering rewrite specified by the R template argument.
     */
    template <class AboveType, class BelowType, template <class, class> class R>
    void bindAboveBelow(MemoLogicalNodeId nodeMemoId, LogicalRewriteType rule);

    /**
     * Attempts to perform a simple rewrite specified by the R template argument.
     */
    template <class Type, template <class> class R>
    void bindSingleNode(MemoLogicalNodeId nodeMemoId, LogicalRewriteType rule);

    void registerRewrite(LogicalRewriteType rewriteType, RewriteFn fn);
    void initializeRewrites();

    static RewriteSet _explorationSet;
    static RewriteSet _substitutionSet;

    const RewriteSet _activeRewriteSet;

    // For standalone logical rewrite phase, keeps track of which groups still have rewrites
    // pending.
    std::set<int> _groupsPending;

    // We don't own those:
    Memo& _memo;
    PrefixId& _prefixId;
    const QueryHints& _hints;
    const PathToIntervalFn& _pathToInterval;

    RewriteFnMap _rewriteMap;

    // Contains the set of top-level index fields for a given scanDef. For example "a.b" is encoded
    // as "a". This is used to constrain the possible splits of a sargable node.
    opt::unordered_map<std::string, opt::unordered_set<FieldNameType>> _indexFieldPrefixMap;

    // Track number of times a SargableNode at a given position in the memo has been split.
    opt::unordered_map<MemoLogicalNodeId, size_t, NodeIdHash> _sargableSplitCountMap;

    // Indicates whether we should only use the heuristic CE during rewrites.
    const bool _useHeuristicCE;
};


}  // namespace mongo::optimizer::cascades
