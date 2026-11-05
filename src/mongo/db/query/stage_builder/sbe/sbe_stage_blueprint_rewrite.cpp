// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_rewrite.h"

#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {
namespace {
/**
 * A FIFO queue of blueprint nodes that holds each node at most once.
 */
class NodeQueue {
public:
    /**
     * Adds 'node' to the back of the queue, unless it is already in the queue.
     */
    void push(SbBlueprintNodeIdx node) {
        if (_inQueue.insert(node).second) {
            _queue.push_back(node);
        }
    }

    /**
     * Removes and returns the node at the front of the queue, which must not be empty.
     */
    SbBlueprintNodeIdx pop() {
        auto node = _queue.front();
        _queue.pop_front();
        _inQueue.erase(node);
        return node;
    }

    bool empty() const {
        return _queue.empty();
    }

private:
    std::deque<SbBlueprintNodeIdx> _queue;

    // The nodes in '_queue'.
    absl::flat_hash_set<SbBlueprintNodeIdx> _inQueue;
};

/**
 * Returns the greatest depth of 'rules', or 0 if there are none.
 */
size_t maxDepth(const SbBlueprintRewriteRules& rules) {
    size_t depth = 0;
    for (const auto& rule : rules) {
        tassert(13821600,
                "A blueprint rewrite rule must have a depth of at least 1",
                rule->depth() >= 1);
        depth = std::max(depth, rule->depth());
    }
    return depth;
}
}  // namespace

/**
 * Main rewrite engine. This class owns the tree while it is rewritten and maintains a work queue,
 * which starts with every node, children before parents. When a node's children change, the node
 * is queued again, along with the nodes above it that are close enough for a rule to see the
 * change. Nodes that rules add are queued too.
 */
class SbBlueprintRewriter {
public:
    SbBlueprintRewriter(SbBlueprint* blueprint,
                        SbBlueprintSlotUsage* slotUsage,
                        SbBlueprintNodeIdx root,
                        const SbBlueprintRewriteRules& rules,
                        size_t maxFirings);

    SbBlueprintRewriteResult run(OperationContext* opCtx);

    // The tree operations behind SbDetachedNode and SbBlueprintRewriteContext.

    void enqueue(SbBlueprintNodeIdx node) {
        _queue.push(node);
    }

    SbBlueprintNode& node(SbBlueprintNodeIdx idx) {
        return (*_blueprint)[idx];
    }

    SbDetachedNode add(SbBlueprintNode node, std::span<SbDetachedNode> children);

    /**
     * Moves the child of 'parent' at position 'childIdx' out of the tree.
     */
    SbDetachedNode takeChild(SbBlueprintNodeIdx parent, size_t childIdx) {
        auto child = std::exchange(*childField(node(parent), childIdx), _emptyChild);
        _parentLinks.erase(child);
        return SbDetachedNode{*this, child};
    }

    /**
     * Makes 'child' the child of 'parent' at position 'childIdx', deleting the child it replaces,
     * and queues 'parent'.
     */
    void setChild(SbBlueprintNodeIdx parent, size_t childIdx, SbDetachedNode child) {
        // The child being replaced goes out of scope here and is deleted.
        auto replaced = takeChild(parent, childIdx);
        link(parent, childIdx, child.release());
        _queue.push(parent);
    }

    /**
     * Deletes 'subtree' and everything under it.
     */
    void destroy(SbBlueprintNodeIdx subtree);

private:
    // A node's parent and its position among the parent's children.
    struct ParentLink {
        SbBlueprintNodeIdx parent;

        // Which of 'parent's children the node is, not the node's own index. For the link stored
        // under node 'n', *childField(parent, childIdx) == n.
        size_t childIdx;
    };

    /**
     * Returns false for a queued node that a rule has since deleted, or one that a rule left out
     * of the tree by writing a child field directly. Lowering fails on the latter.
     */
    bool inTree(SbBlueprintNodeIdx node) const {
        return !std::holds_alternative<SbBlueprintLowered>((*_blueprint)[node]) &&
            _parentLinks.contains(node);
    }

    /**
     * Queues 'node', whose children changed, and the nodes above it, '_maxDepth' nodes in all. The
     * walk stops at the root holder, which is never rewritten.
     */
    void enqueueWithAncestors(SbBlueprintNodeIdx node) {
        for (size_t i = 0; i < _maxDepth && node != _rootHolder; ++i) {
            _queue.push(node);
            node = _parentLinks.at(node).parent;
        }
    }

    /**
     * Applies the rules at 'node', and then at each node that replaces it, until none fires.
     */
    void applyRules(OperationContext* opCtx, SbBlueprintNodeIdx node);

    /**
     * Puts 'node' where 'link' points.
     */
    void put(const ParentLink& link, SbDetachedNode node) {
        this->link(link.parent, link.childIdx, node.release());
    }

    /**
     * Makes 'child' the child of 'parent' at position 'childIdx'.
     */
    void link(SbBlueprintNodeIdx parent, size_t childIdx, SbBlueprintNodeIdx child) {
        *childField(node(parent), childIdx) = child;
        _parentLinks[child] = ParentLink{parent, childIdx};
    }

    SbBlueprint* _blueprint;

    // The slot summary the caller supplied, or null. Rules keep it up to date.
    SbBlueprintSlotUsage* _slotUsage;

    // Placeholder for a child field whose child a rule took out.
    const SbBlueprintNodeIdx _emptyChild;

    // Holds the root of the tree as its only child, so that the root has a parent like every
    // other node. It is never rewritten, and is deleted when the rewrite finishes.
    const SbBlueprintNodeIdx _rootHolder;

    const SbBlueprintRewriteRules& _rules;

    // The greatest depth of '_rules'.
    const size_t _maxDepth;

    const size_t _maxFirings;
    size_t _firings{0};

    // Each node's parent, by node index. The root's parent is the root holder. A node that is out
    // of the tree has none.
    absl::flat_hash_map<SbBlueprintNodeIdx, ParentLink> _parentLinks;

    // The nodes left to rewrite.
    NodeQueue _queue;
};

SbBlueprintRewriter::SbBlueprintRewriter(SbBlueprint* blueprint,
                                         SbBlueprintSlotUsage* slotUsage,
                                         SbBlueprintNodeIdx root,
                                         const SbBlueprintRewriteRules& rules,
                                         size_t maxFirings)
    : _blueprint(blueprint),
      _slotUsage(slotUsage),
      _emptyChild(blueprint->add(SbBlueprintLowered{})),
      // An empty LimitSkip serves as the root holder.
      _rootHolder(blueprint->add(SbBlueprintLimitSkip{.child = root})),
      _rules(rules),
      _maxDepth(maxDepth(rules)),
      _maxFirings(maxFirings) {
    // Record each node's parent, and queue the nodes children before parents. The root holder
    // records the root's parent but is not queued.
    std::vector<SbBlueprintNodeIdx> order;
    std::vector<SbBlueprintNodeIdx> stack{_rootHolder};
    while (!stack.empty()) {
        auto node = stack.back();
        stack.pop_back();
        if (node != _rootHolder) {
            order.push_back(node);
        }
        size_t childIdx = 0;
        forEachChild((*_blueprint)[node], [&](SbBlueprintNodeIdx& child) {
            _parentLinks[child] = ParentLink{node, childIdx++};
            stack.push_back(child);
        });
    }
    std::for_each(order.rbegin(), order.rend(), [&](auto node) { _queue.push(node); });
}

SbBlueprintRewriteResult SbBlueprintRewriter::run(OperationContext* opCtx) {
    while (!_queue.empty() && _firings < _maxFirings) {
        auto node = _queue.pop();
        // Skips stale entries for nodes deleted after they were queued.
        if (inTree(node)) {
            applyRules(opCtx, node);
        }
    }
    // Reaching the budget counts as not converging, since more rules may have fired.
    const bool converged = _firings < _maxFirings;
    LOGV2_DEBUG(13627007,
                2,
                "Applied SBE blueprint rewrite rules",
                "firings"_attr = _firings,
                "converged"_attr = converged);

    // The root holder is not part of the plan, and lowering requires every node to be lowered.
    auto root = *childField(node(_rootHolder), 0);
    node(_rootHolder) = SbBlueprintLowered{};
    return {root, _firings, converged};
}

SbDetachedNode SbBlueprintRewriter::add(SbBlueprintNode node, std::span<SbDetachedNode> children) {
    tassert(13627019,
            "A new blueprint node must be given exactly one child for each of its child fields",
            children.size() == numChildren(node));
    auto added = _blueprint->add(std::move(node));
    for (size_t childIdx = 0; childIdx < children.size(); ++childIdx) {
        link(added, childIdx, children[childIdx].release());
    }
    // A rule adds a node's children before the node, so they are queued before it.
    _queue.push(added);
    return SbDetachedNode{*this, added};
}

void SbBlueprintRewriter::destroy(SbBlueprintNodeIdx subtree) {
    std::vector<SbBlueprintNodeIdx> stack{subtree};
    while (!stack.empty()) {
        auto dropped = stack.back();
        stack.pop_back();
        // The node that fills empty child fields is already SbBlueprintLowered.
        if (std::holds_alternative<SbBlueprintLowered>(node(dropped))) {
            continue;
        }
        forEachChild(node(dropped), [&](SbBlueprintNodeIdx& child) { stack.push_back(child); });
        _parentLinks.erase(dropped);
        node(dropped) = SbBlueprintLowered{};
    }
}

void SbBlueprintRewriter::applyRules(OperationContext* opCtx, SbBlueprintNodeIdx node) {
    SbBlueprintRewriteContext ctx{*this, _slotUsage};
    while (_firings < _maxFirings) {
        // Take 'node' out of the tree, as a rule takes a child out of its parent.
        const auto link = _parentLinks.at(node);
        tassert(13627021,
                "Blueprint node's parent link does not point at the node",
                *childField(this->node(link.parent), link.childIdx) == node);
        auto detached = takeChild(link.parent, link.childIdx);

        boost::optional<SbDetachedNode> result;
        for (const auto& rule : _rules) {
            result = rule->apply(detached, ctx);
            if (result) {
                LOGV2_DEBUG(13627018,
                            5,
                            "Applied SBE blueprint rewrite rule",
                            "rule"_attr = rule->name(),
                            "node"_attr = node.value);
                break;
            }
        }
        if (!result) {
            // Nothing changed, so 'node' goes back without queueing its parent.
            put(link, std::move(detached));
            return;
        }

        ++_firings;
        opCtx->checkForInterrupt();
        node = result->get();
        put(link, std::move(*result));
        // Queue the nodes above, up to the greatest rule depth, in case the change lets a rule
        // that looks that many levels down fire on one of them.
        enqueueWithAncestors(link.parent);
        // 'detached' goes out of scope here and deletes the old node unless the rule kept it.
    }
}

void SbBlueprintRewriteContext::enqueue(SbBlueprintNodeIdx node) {
    _rewriter.enqueue(node);
}

const SbBlueprintNode& SbBlueprintRewriteContext::node(SbBlueprintNodeIdx idx) const {
    return _rewriter.node(idx);
}

SbDetachedNode SbBlueprintRewriteContext::add(SbBlueprintNode node,
                                              std::span<SbDetachedNode> children) {
    return _rewriter.add(std::move(node), children);
}

SbDetachedNode& SbDetachedNode::operator=(SbDetachedNode&& other) noexcept {
    if (this != &other) {
        if (_owned) {
            _rewriter->destroy(_idx);
        }
        _rewriter = other._rewriter;
        _idx = other._idx;
        _owned = std::exchange(other._owned, false);
    }
    return *this;
}

SbDetachedNode::~SbDetachedNode() {
    if (_owned) {
        _rewriter->destroy(_idx);
    }
}

SbBlueprintNode& SbDetachedNode::operator*() const {
    return _rewriter->node(get());
}

SbDetachedNode SbDetachedNode::takeChild(size_t childIdx) {
    return _rewriter->takeChild(get(), childIdx);
}

void SbDetachedNode::setChild(size_t childIdx, SbDetachedNode child) {
    _rewriter->setChild(get(), childIdx, std::move(child));
}

SbBlueprintRewriteResult rewriteSbeBlueprint(OperationContext* opCtx,
                                             SbBlueprint* blueprint,
                                             SbBlueprintSlotUsage* slotUsage,
                                             SbBlueprintNodeIdx root,
                                             const SbBlueprintRewriteRules& rules,
                                             size_t maxFirings) {
    return SbBlueprintRewriter{blueprint, slotUsage, root, rules, maxFirings}.run(opCtx);
}

void checkSbeBlueprintRewriteConverged(const SbBlueprintRewriteResult& result) {
    // A plan that does not converge is still valid and runnable, so this only tasserts in debug
    // builds.
    if (!result.converged) {
        if constexpr (kDebugBuild) {
            tasserted(13627005, "SBE blueprint rewrite did not converge");
        }
        LOGV2_WARNING(
            13627006, "SBE blueprint rewrite did not converge", "firings"_attr = result.firings);
    }
}
}  // namespace mongo::stage_builder
