// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_slot_usage.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

/**
 * Stage Builder Blueprint Rewrite Driver
 *
 * A rule works on the tree as if each node owned its children through a std::unique_ptr. The node
 * being rewritten arrives as an owning SbDetachedNode. Its takeChild() moves a child out, like
 * std::move from a unique_ptr field, and its setChild() gives the node a new child, like assigning
 * to one. A detached node that ends up with no owner is deleted along with its subtree.
 *
 * It is the rules' responsibility to maintain the tree structure. Lowering does a final check and
 * tasserts if the tree is invalid.
 *
 * A caller can supply a summary of which node defines each slot and how many times each slot is
 * read, which every rule sees through SbBlueprintRewriteContext::slotUsage(). The engine never
 * updates it: a rule that adds or removes slot definitions or reads must update it itself.
 */

namespace mongo::stage_builder {
class SbBlueprintRewriter;

/**
 * A blueprint node that is not in the tree, and the subtree under it. Like a std::unique_ptr, it
 * deletes the subtree when it goes out of scope unless it was given to a new parent.
 */
class SbDetachedNode {
public:
    SbDetachedNode(SbBlueprintRewriter& rewriter, SbBlueprintNodeIdx idx)
        : _rewriter(&rewriter), _idx(idx) {}

    SbDetachedNode(SbDetachedNode&& other) noexcept
        : _rewriter(other._rewriter),
          _idx(other._idx),
          _owned(std::exchange(other._owned, false)) {}

    SbDetachedNode& operator=(SbDetachedNode&& other) noexcept;
    ~SbDetachedNode();

    SbDetachedNode(const SbDetachedNode&) = delete;
    SbDetachedNode& operator=(const SbDetachedNode&) = delete;

    /**
     * Returns the node's index without giving up ownership.
     */
    SbBlueprintNodeIdx get() const {
        tassert(13627017, "Detached blueprint node has been moved from", _owned);
        return _idx;
    }

    /**
     * The node itself. A rule may change any field except the child fields, which it changes
     * through takeChild() and setChild().
     */
    SbBlueprintNode& operator*() const;

    /**
     * Moves the child at position 'childIdx' out of this node and returns an owner for it. The
     * position must be filled again with setChild() before the rule returns.
     */
    SbDetachedNode takeChild(size_t childIdx);

    /**
     * Makes 'child' the child at position 'childIdx', deleting the child it replaces, if any.
     */
    void setChild(size_t childIdx, SbDetachedNode child);

private:
    // The rewriter stores a released node in a child field or makes it the root.
    friend class SbBlueprintRewriter;

    // Gives up ownership and returns the node's index.
    SbBlueprintNodeIdx release() {
        auto idx = get();
        _owned = false;
        return idx;
    }

    SbBlueprintRewriter* _rewriter;
    SbBlueprintNodeIdx _idx;

    // False once the node has been given to a parent or moved from.
    bool _owned{true};
};

/**
 * Context passed to rewrite rules that run in the engine.
 */
class SbBlueprintRewriteContext {
public:
    SbBlueprintRewriteContext(SbBlueprintRewriter& rewriter, SbBlueprintSlotUsage* slotUsage)
        : _rewriter(rewriter), _slotUsage(slotUsage) {}

    /**
     * Returns the node at 'idx', for reading. A rule reaches the nodes below the one it rewrites
     * this way, and takes them out of the tree with takeChild() to change them.
     */
    const SbBlueprintNode& node(SbBlueprintNodeIdx idx) const;

    /**
     * Adds 'node' to the blueprint and returns an owner for it. 'children' fill its child fields,
     * in the order forEachChild() visits them, and whatever the fields held before is ignored.
     * The node is queued to be rewritten.
     */
    template <typename... Children>
    SbDetachedNode add(SbBlueprintNode node, Children... children) {
        std::array<SbDetachedNode, sizeof...(Children)> childArray{std::move(children)...};
        return add(std::move(node), std::span<SbDetachedNode>{childArray});
    }

    /**
     * Queues 'node' to be rewritten again. A rule calls this when it changes something a rule
     * matching on 'node' depends on, such as removing the last read of a slot 'node' defines.
     */
    void enqueue(SbBlueprintNodeIdx node);

    /**
     * Returns the slot summary of the tree that the caller of rewriteSbeBlueprint() supplied. A
     * rule that changes slot definitions or reads must update it.
     */
    SbBlueprintSlotUsage& slotUsage() {
        tassert(13627020, "No slot summary was supplied to the blueprint rewrite", _slotUsage);
        return *_slotUsage;
    }

private:
    SbDetachedNode add(SbBlueprintNode node, std::span<SbDetachedNode> children);

    SbBlueprintRewriter& _rewriter;
    SbBlueprintSlotUsage* _slotUsage;
};

/**
 * Interface for a single rewrite rule that replaces a single subtree.
 */
class SbBlueprintRewriteRule {
public:
    virtual ~SbBlueprintRewriteRule() = default;

    virtual std::string_view name() const = 0;

    /**
     * How many levels below its node the rule looks to decide whether to fire, at least 1. A rule
     * that reads only its node and the node's children returns 1. When a node's children change,
     * the engine rewrites the node again, and the nodes up to 'depth() - 1' levels above it.
     */
    virtual size_t depth() const = 0;

    /**
     * Attempts to rewrite 'node', which has been taken out of the tree. Returns boost::none if the
     * rule did not fire and changed nothing. Otherwise returns the node to put in place of 'node',
     * which may be 'node' itself.
     */
    virtual boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                                  SbBlueprintRewriteContext& ctx) = 0;
};

using SbBlueprintRewriteRules = std::vector<std::unique_ptr<SbBlueprintRewriteRule>>;

struct SbBlueprintRewriteResult {
    // The root of the rewritten tree.
    SbBlueprintNodeIdx root;

    // How many times a rule fired.
    size_t firings;

    // False if the rewrite stopped at the firing budget, when more rules may have fired. The tree
    // is still valid and can be lowered.
    bool converged;
};

/**
 * Applies 'rules' to the tree rooted at 'root' until none fires or 'maxFirings' rules have fired.
 * 'slotUsage', if not null, is the slot summary of the tree, which the rules keep up to date.
 */
SbBlueprintRewriteResult rewriteSbeBlueprint(OperationContext* opCtx,
                                             SbBlueprint* blueprint,
                                             SbBlueprintSlotUsage* slotUsage,
                                             SbBlueprintNodeIdx root,
                                             const SbBlueprintRewriteRules& rules,
                                             size_t maxFirings);

/**
 * Checks that 'result' converged. A plan that does not converge is still valid and runnable, so
 * this only tasserts in debug builds and logs a warning otherwise. Tests that stop at a firing
 * budget on purpose do not call it.
 */
void checkSbeBlueprintRewriteConverged(const SbBlueprintRewriteResult& result);
}  // namespace mongo::stage_builder
