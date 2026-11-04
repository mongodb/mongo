// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_rewrite.h"

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/query/stage_builder/sbe/sbe_stage_lowering.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <absl/container/flat_hash_set.h>

namespace mongo::stage_builder {
namespace {
/**
 * Tests for the blueprint rewrite driver, using test-local rules.
 */
class SbBlueprintRewriteTest : public GoldenSbeExprBuilderTestFixture {
protected:
    SbBlueprint& blueprint() {
        return _state->blueprint();
    }

    const SbBlueprintNode& node(SbBlueprintNodeIdx idx) {
        return blueprint()[idx];
    }

    SbBlueprintNodeIdx coscan() {
        return blueprint().add(SbBlueprintCoScan{});
    }

    SbBlueprintNodeIdx limitOne(SbBlueprintNodeIdx child) {
        return blueprint().add(SbBlueprintLimitSkip{
            .child = child, .limitExpr = SbExprBuilder{*_state}.makeInt64Constant(1)});
    }

    SbBlueprintNodeIdx filterTrue(SbBlueprintNodeIdx child) {
        return blueprint().add(SbBlueprintFilter{
            .child = child, .condition = SbExprBuilder{*_state}.makeBoolConstant(true)});
    }

    SbBlueprintNodeIdx rewrite(SbBlueprintNodeIdx root,
                               std::unique_ptr<SbBlueprintRewriteRule> rule,
                               size_t maxFirings = 10000) {
        SbBlueprintRewriteRules rules;
        rules.push_back(std::move(rule));
        return rewriteSbeBlueprint(_state->opCtx, &blueprint(), root, rules, maxFirings);
    }
};

// Replaces every filter with its child.
class DropFiltersRule : public SbBlueprintRewriteRule {
public:
    std::string_view name() const override {
        return "dropFilters";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        if (std::holds_alternative<SbBlueprintFilter>(*node)) {
            return node.takeChild(0);
        }
        return boost::none;
    }
};

TEST_F(SbBlueprintRewriteTest, ReplacingTheRootReturnsTheNewRoot) {
    auto leaf = coscan();
    auto filter = filterTrue(leaf);

    auto newRoot = rewrite(filter, std::make_unique<DropFiltersRule>());

    ASSERT_EQ(newRoot, leaf);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(filter)));
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

TEST_F(SbBlueprintRewriteTest, ReplacingTheRootRepeatedly) {
    // Each firing replaces the current root with its child, down to the leaf.
    auto leaf = coscan();
    auto root = filterTrue(filterTrue(filterTrue(leaf)));

    auto newRoot = rewrite(root, std::make_unique<DropFiltersRule>());

    ASSERT_EQ(newRoot, leaf);
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

TEST_F(SbBlueprintRewriteTest, ReplacingANonRootNodeRelinksItsParent) {
    auto leaf = coscan();
    auto filter = filterTrue(leaf);
    auto root = limitOne(filter);

    auto newRoot = rewrite(root, std::make_unique<DropFiltersRule>());

    ASSERT_EQ(newRoot, root);
    ASSERT_EQ(std::get<SbBlueprintLimitSkip>(node(root)).child, leaf);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(filter)));
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

// Wraps each node of type 'Node' in a LimitSkip, once.
template <typename Node>
class WrapOnceRule : public SbBlueprintRewriteRule {
public:
    explicit WrapOnceRule(StageBuilderState& state) : _state(state) {}

    std::string_view name() const override {
        return "wrapOnce";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        if (!std::holds_alternative<Node>(*node) || !_wrapped.insert(node.get().value).second) {
            return boost::none;
        }
        return ctx.add(
            SbBlueprintLimitSkip{.limitExpr = SbExprBuilder{_state}.makeInt64Constant(1)},
            std::move(node));
    }

private:
    StageBuilderState& _state;
    absl::flat_hash_set<uint32_t> _wrapped;
};

TEST_F(SbBlueprintRewriteTest, WrappingANonRootNode) {
    auto leaf = coscan();
    auto root = filterTrue(leaf);

    auto newRoot = rewrite(root, std::make_unique<WrapOnceRule<SbBlueprintCoScan>>(*_state));

    ASSERT_EQ(newRoot, root);
    auto wrapper = std::get<SbBlueprintFilter>(node(root)).child;
    ASSERT_EQ(std::get<SbBlueprintLimitSkip>(node(wrapper)).child, leaf);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintCoScan>(node(leaf)));
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

TEST_F(SbBlueprintRewriteTest, AddingManyNodesDuringTheWalk) {
    // The result is not lowered because lowering recurses and the tree is too deep for small
    // stacks.
    auto root = coscan();
    for (int i = 0; i < 1000; ++i) {
        root = filterTrue(root);
    }

    auto newRoot = rewrite(root, std::make_unique<WrapOnceRule<SbBlueprintFilter>>(*_state));

    // Every filter ends up under its own LimitSkip, all the way down to the coscan.
    auto current = newRoot;
    for (int i = 0; i < 1000; ++i) {
        auto filter = std::get<SbBlueprintLimitSkip>(node(current)).child;
        current = std::get<SbBlueprintFilter>(node(filter)).child;
    }
    ASSERT_TRUE(std::holds_alternative<SbBlueprintCoScan>(node(current)));
}

// This is testing that all nodes `add()`ed during rewrites get enqueued and visited by the engine.
// Wraps the first coscan it sees in a new filter under a new LimitSkip.
class WrapInLimitAndFilterOnceRule : public SbBlueprintRewriteRule {
public:
    explicit WrapInLimitAndFilterOnceRule(StageBuilderState& state) : _state(state) {}

    std::string_view name() const override {
        return "wrapInLimitAndFilterOnce";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        if (!std::holds_alternative<SbBlueprintCoScan>(*node) || _fired) {
            return boost::none;
        }
        _fired = true;
        SbExprBuilder b{_state};
        auto filter =
            ctx.add(SbBlueprintFilter{.condition = b.makeBoolConstant(true)}, std::move(node));
        return ctx.add(SbBlueprintLimitSkip{.limitExpr = b.makeInt64Constant(1)},
                       std::move(filter));
    }

private:
    StageBuilderState& _state;
    bool _fired{false};
};

TEST_F(SbBlueprintRewriteTest, NodesAddedUnderTheReplacementAreRewritten) {
    // The new filter is below the node that replaces the coscan, so it is rewritten only because
    // adding it queued it.
    auto leaf = coscan();
    auto root = limitOne(leaf);

    SbBlueprintRewriteRules rules;
    rules.push_back(std::make_unique<WrapInLimitAndFilterOnceRule>(*_state));
    rules.push_back(std::make_unique<DropFiltersRule>());
    auto newRoot = rewriteSbeBlueprint(_state->opCtx, &blueprint(), root, rules, 10000);

    ASSERT_EQ(newRoot, root);
    auto wrapper = std::get<SbBlueprintLimitSkip>(node(root)).child;
    ASSERT_EQ(std::get<SbBlueprintLimitSkip>(node(wrapper)).child, leaf);
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

// Replaces a union with its first child, discarding the others.
class KeepFirstUnionChildRule : public SbBlueprintRewriteRule {
public:
    std::string_view name() const override {
        return "keepFirstUnionChild";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        if (std::holds_alternative<SbBlueprintUnion>(*node)) {
            return node.takeChild(0);
        }
        return boost::none;
    }
};

TEST_F(SbBlueprintRewriteTest, DiscardedSubtreeIsRemoved) {
    auto left = coscan();
    auto rightLeaf = coscan();
    auto right = limitOne(rightLeaf);
    auto u = blueprint().add(SbBlueprintUnion{
        .nodeId = 1, .children = {left, right}, .inputSlots = {{10}, {11}}, .outputSlots = {20}});

    auto newRoot = rewrite(u, std::make_unique<KeepFirstUnionChildRule>());

    ASSERT_EQ(newRoot, left);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(u)));
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(right)));
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(rightLeaf)));
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

// Replaces a filter with two new LimitSkips over the filter's grandchild.
class AdoptGrandchildRule : public SbBlueprintRewriteRule {
public:
    explicit AdoptGrandchildRule(StageBuilderState& state) : _state(state) {}

    std::string_view name() const override {
        return "adoptGrandchild";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        if (!std::holds_alternative<SbBlueprintFilter>(*node)) {
            return boost::none;
        }
        auto grandchild = node.takeChild(0).takeChild(0);
        auto inner =
            ctx.add(SbBlueprintLimitSkip{.limitExpr = SbExprBuilder{_state}.makeInt64Constant(1)},
                    std::move(grandchild));
        return ctx.add(
            SbBlueprintLimitSkip{.limitExpr = SbExprBuilder{_state}.makeInt64Constant(1)},
            std::move(inner));
    }

private:
    StageBuilderState& _state;
};

TEST_F(SbBlueprintRewriteTest, NewNodesCanKeepANodeFromDeeperDown) {
    auto leaf = coscan();
    auto middle = limitOne(leaf);
    auto filter = filterTrue(middle);
    auto root = limitOne(filter);

    auto newRoot = rewrite(root, std::make_unique<AdoptGrandchildRule>(*_state));

    // The filter and the node between it and its grandchild are deleted.
    ASSERT_EQ(newRoot, root);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(filter)));
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(middle)));
    auto outer = std::get<SbBlueprintLimitSkip>(node(root)).child;
    auto inner = std::get<SbBlueprintLimitSkip>(node(outer)).child;
    ASSERT_EQ(std::get<SbBlueprintLimitSkip>(node(inner)).child, leaf);
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

// Moves a filter below the LimitSkip under it, reusing both nodes.
class SwapFilterAndLimitRule : public SbBlueprintRewriteRule {
public:
    std::string_view name() const override {
        return "swapFilterAndLimit";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        auto* filter = std::get_if<SbBlueprintFilter>(&*node);
        if (!filter || !std::holds_alternative<SbBlueprintLimitSkip>(ctx.node(filter->child))) {
            return boost::none;
        }
        auto limit = node.takeChild(0);
        node.setChild(0, limit.takeChild(0));
        limit.setChild(0, std::move(node));
        return limit;
    }
};

TEST_F(SbBlueprintRewriteTest, RearrangedNodesAreRewrittenAgain) {
    // Each swap moves the filter one LimitSkip down, where it is found again and swapped again
    // until it reaches the leaf.
    auto leaf = coscan();
    auto filter = filterTrue(limitOne(limitOne(limitOne(leaf))));

    auto newRoot = rewrite(filter, std::make_unique<SwapFilterAndLimitRule>());

    auto current = newRoot;
    for (int i = 0; i < 3; ++i) {
        current = std::get<SbBlueprintLimitSkip>(node(current)).child;
    }
    ASSERT_EQ(current, filter);
    ASSERT_EQ(std::get<SbBlueprintFilter>(node(filter)).child, leaf);
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

// Replaces a unique over two LimitSkips with its child. It looks at its grandchild, so its depth
// is 2.
class DropUniqueOverTwoLimitsRule : public SbBlueprintRewriteRule {
public:
    std::string_view name() const override {
        return "dropUniqueOverTwoLimits";
    }

    size_t depth() const override {
        return 2;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        auto* unique = std::get_if<SbBlueprintUnique>(&*node);
        if (!unique) {
            return boost::none;
        }
        auto* limit = std::get_if<SbBlueprintLimitSkip>(&ctx.node(unique->child));
        if (!limit || !std::holds_alternative<SbBlueprintLimitSkip>(ctx.node(limit->child))) {
            return boost::none;
        }
        return node.takeChild(0);
    }
};

TEST_F(SbBlueprintRewriteTest, ChangeTwoLevelsDownRewritesTheNodeAgain) {
    // unique -> filter -> limit -> limit -> coscan becomes limit -> limit -> filter -> coscan.
    // The filter moves below each LimitSkip in turn.
    auto leaf = coscan();
    auto lower = limitOne(leaf);
    auto upper = limitOne(lower);
    auto filter = filterTrue(upper);
    auto unique = blueprint().add(SbBlueprintUnique{.child = filter, .keys = {10}});

    SbBlueprintRewriteRules rules;
    rules.push_back(std::make_unique<SwapFilterAndLimitRule>());
    rules.push_back(std::make_unique<DropUniqueOverTwoLimitsRule>());
    auto newRoot = rewriteSbeBlueprint(_state->opCtx, &blueprint(), unique, rules, 10000);

    ASSERT_EQ(newRoot, upper);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLowered>(node(unique)));
    ASSERT_EQ(std::get<SbBlueprintLimitSkip>(node(upper)).child, lower);
    ASSERT_EQ(std::get<SbBlueprintLimitSkip>(node(lower)).child, filter);
    ASSERT_EQ(std::get<SbBlueprintFilter>(node(filter)).child, leaf);
    lowerSbeBlueprint(blueprint(), newRoot, *_state);
}

// Fires 'times' times in place, then stops.
class FireNTimesRule : public SbBlueprintRewriteRule {
public:
    explicit FireNTimesRule(size_t times) : _left(times) {}

    std::string_view name() const override {
        return "fireNTimes";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext&) override {
        if (_left == 0) {
            return boost::none;
        }
        --_left;
        return std::move(node);
    }

    size_t left() const {
        return _left;
    }

private:
    size_t _left;
};

TEST_F(SbBlueprintRewriteTest, InPlaceRuleFiresUntilItStops) {
    auto root = limitOne(coscan());
    SbBlueprintRewriteRules rules;
    rules.push_back(std::make_unique<FireNTimesRule>(8));

    ASSERT_EQ(rewriteSbeBlueprint(_state->opCtx, &blueprint(), root, rules, 10000), root);
    ASSERT_EQ(static_cast<FireNTimesRule&>(*rules[0]).left(), 0);
}

// The rules below misuse the API. The driver tasserts on misuse it can detect cheaply. Checking the
// whole tree after every rewrite would cost O(n) each time, so the rest is left to lowering.

// Wraps a filter's child by writing its child field directly instead of taking it, so the child
// is left with two parents.
class ShareChildRule : public SbBlueprintRewriteRule {
public:
    explicit ShareChildRule(StageBuilderState& state) : _state(state) {}

    std::string_view name() const override {
        return "shareChild";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        auto* filter = std::get_if<SbBlueprintFilter>(&*node);
        if (!filter || _fired) {
            return boost::none;
        }
        _fired = true;
        auto child = filter->child;
        auto wrapper =
            ctx.add(SbBlueprintLimitSkip{.limitExpr = SbExprBuilder{_state}.makeInt64Constant(1)},
                    ctx.add(SbBlueprintCoScan{}));
        std::get<SbBlueprintLimitSkip>(*wrapper).child = child;
        node.setChild(0, std::move(wrapper));
        return std::move(node);
    }

private:
    StageBuilderState& _state;
    bool _fired{false};
};

// Wraps a filter, then returns the moved-from handle instead of the new node.
class ReturnMovedFromNodeRule : public SbBlueprintRewriteRule {
public:
    explicit ReturnMovedFromNodeRule(StageBuilderState& state) : _state(state) {}

    std::string_view name() const override {
        return "returnMovedFromNode";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        if (!std::holds_alternative<SbBlueprintFilter>(*node)) {
            return boost::none;
        }
        ctx.add(SbBlueprintLimitSkip{.limitExpr = SbExprBuilder{_state}.makeInt64Constant(1)},
                std::move(node));
        return std::move(node);  // NOLINT(bugprone-use-after-move)
    }

private:
    StageBuilderState& _state;
};

// Adds a filter without giving it a child.
class AddChildlessFilterRule : public SbBlueprintRewriteRule {
public:
    std::string_view name() const override {
        return "addChildlessFilter";
    }

    size_t depth() const override {
        return 1;
    }

    boost::optional<SbDetachedNode> apply(SbDetachedNode& node,
                                          SbBlueprintRewriteContext& ctx) override {
        return ctx.add(SbBlueprintFilter{});
    }
};

using SbBlueprintRewriteDeathTest = SbBlueprintRewriteTest;

// The coscan that the rule added is still queued, but its parent link no longer points at it.
DEATH_TEST_F(SbBlueprintRewriteDeathTest, SharedChildFailsTheParentLinkCheck, "13627021") {
    rewrite(filterTrue(coscan()), std::make_unique<ShareChildRule>(*_state));
}

DEATH_TEST_F(SbBlueprintRewriteDeathTest, RuleCannotUseAMovedFromNode, "13627017") {
    rewrite(filterTrue(coscan()), std::make_unique<ReturnMovedFromNodeRule>(*_state));
}

DEATH_TEST_F(SbBlueprintRewriteDeathTest, NewNodeNeedsOneChildPerChildField, "13627019") {
    rewrite(coscan(), std::make_unique<AddChildlessFilterRule>());
}

#ifdef MONGO_CONFIG_DEBUG_BUILD
DEATH_TEST_F(SbBlueprintRewriteDeathTest, RuleOverTheFiringBudgetFails, "13627005") {
    rewrite(limitOne(coscan()), std::make_unique<FireNTimesRule>(8), 5 /* maxFirings */);
}
#endif
}  // namespace
}  // namespace mongo::stage_builder
