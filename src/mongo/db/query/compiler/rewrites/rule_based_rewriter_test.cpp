/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/rewrites/rule_based_rewriter.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/algorithm/string/case_conv.hpp>

namespace mongo::rule_based_rewrites {
namespace {

// Simple test for the base class that applies transformations to strings.
class TestRewriteContext : public RewriteContext<TestRewriteContext, std::string> {
public:
    using Rules = std::vector<Rule<TestRewriteContext>>;

    TestRewriteContext(std::vector<std::string>& strings, Rules rules)
        : _container(strings), _itr(_container.begin()), rules(std::move(rules)) {}

    bool hasMore() const final {
        return _itr != _container.end();
    }

    void advance() final {
        if (hasMore()) {
            ++_itr;
        }
    }

    std::string& current() final {
        return *_itr;
    }
    const std::string& current() const final {
        return *_itr;
    }

    void enqueueRules() final {
        addRules(rules);
    }

    void appendString(std::string str) {
        _container.push_back(str);
    }

private:
    std::vector<std::string>& _container;
    std::vector<std::string>::iterator _itr;
    Rules rules;
};

// Preconditions
bool isHello(TestRewriteContext& ctx) {
    return ctx.current() == "hello";
}
bool isWorld(TestRewriteContext& ctx) {
    return ctx.current() == "world";
}
bool alwaysTrue(TestRewriteContext&) {
    return true;
}

// Transforms
bool noop(TestRewriteContext& ctx) {
    return false;
}
bool shouldNeverRun(TestRewriteContext& ctx) {
    MONGO_UNREACHABLE;
}
bool upperCaseTransform(TestRewriteContext& ctx) {
    boost::to_upper(ctx.current());
    return false;
}
bool appendExclamationTransform(TestRewriteContext& ctx) {
    ctx.current() += "!";
    return false;
}

TEST(RuleBasedRewriterTest, RespectPrecondition) {
    std::vector<std::string> strings = {{"hello"}, {"world"}};
    RewriteEngine<TestRewriteContext> engine{{
        strings,
        {{"NEVER_APPLIES", [](TestRewriteContext&) { return false; }, shouldNeverRun, 1}},
    }};

    ASSERT_DOES_NOT_THROW(engine.applyRules());
}

TEST(RuleBasedRewriterTest, RespectZeroOptimizationBudget) {
    std::vector<std::string> strings = {{"hello"}};
    RewriteEngine<TestRewriteContext> engine{
        {
            strings,
            {{"NEVER_APPLIES", alwaysTrue, shouldNeverRun, 1}},
        },
        0 /*maxRewrites*/,
    };

    ASSERT_DOES_NOT_THROW(engine.applyRules());
}

TEST(RuleBasedRewriterTest, RespectNonZeroOptimizationBudget) {
    std::vector<std::string> strings = {{"hello"}};
    RewriteEngine<TestRewriteContext> engine{
        {
            strings,
            {
                {"NOOP1", alwaysTrue, noop, 3},
                {"NOOP2", alwaysTrue, noop, 2},
                {"NEVER_APPLIES", alwaysTrue, shouldNeverRun, 1},
            },
        },
        2 /*maxRewrites*/,
    };

    ASSERT_DOES_NOT_THROW(engine.applyRules());
}

TEST(RuleBasedRewriterTest, ApplyAllRulesBeforeReachingOptimizationBudget) {
    std::vector<std::string> strings = {{"hello"}};
    RewriteEngine<TestRewriteContext> engine{
        {
            strings,
            {
                {"NOOP1", alwaysTrue, noop, 3},
                {"NOOP2", alwaysTrue, noop, 2},
                {"SHOULD_APPLY", alwaysTrue, upperCaseTransform, 1},
            },
        },
        3 /*maxRewrites*/,
    };

    engine.applyRules();

    ASSERT_EQ(strings.size(), 1U);
    ASSERT_EQ(strings[0], "HELLO");
}

TEST(RuleBasedRewriterTest, ApplySingleRule) {
    std::vector<std::string> strings = {{"hello"}, {"world"}};
    RewriteEngine<TestRewriteContext> engine{{
        strings,
        {{"UPPERCASE_HELLO", isHello, upperCaseTransform, 1}},
    }};

    engine.applyRules();

    ASSERT_EQ(strings.size(), 2U);
    ASSERT_EQ(strings[0], "HELLO");
    ASSERT_EQ(strings[1], "world");
}

TEST(RuleBasedRewriterTest, ApplyMultipleRulesDifferentPriorities) {
    std::vector<std::string> strings = {{"hello"}, {"world"}};
    RewriteEngine<TestRewriteContext> engine{{
        strings,
        {
            {"UPPERCASE_HELLO", isHello, upperCaseTransform, 1},
            {"UPPERCASE_WORLD", isWorld, upperCaseTransform, 1},
            // Takes precedence over UPPERCASE_WORLD, which is not applied
            {"ADD_EXCLAMATION_TO_WORLD", isWorld, appendExclamationTransform, 2},
        },
    }};

    engine.applyRules();

    ASSERT_EQ(strings.size(), 2U);
    ASSERT_EQ(strings[0], "HELLO");
    ASSERT_EQ(strings[1], "world!");
}

TEST(RuleBasedRewriterTest, EnqueueAdditionalRulesFromRule) {
    std::vector<std::string> strings = {{"hello"}, {"world"}};

    auto enqueueUpperCaseHello = [&](TestRewriteContext& ctx) {
        ctx.addRules({{"UPPERCASE_HELLO", isHello, upperCaseTransform, 1.5}});
        return false;
    };

    RewriteEngine<TestRewriteContext> engine{{
        strings,
        {
            {"ENQUEUE_UPPERCASE_HELLO", isHello, enqueueUpperCaseHello, 2},
            {"NEVER_APPLIED", isHello, shouldNeverRun, 1},
        },
    }};

    engine.applyRules();

    ASSERT_EQ(strings.size(), 2U);
    ASSERT_EQ(strings[0], "HELLO");
    ASSERT_EQ(strings[1], "world");
}

TEST(RuleBasedRewriterTest, EnqueueHighestPriorityRule) {
    std::vector<std::string> strings = {{"hello"}, {"world"}};

    auto enqueueHighestPriorityRule = [&](TestRewriteContext& ctx) {
        ctx.current() = "not hello";
        ctx.addRules({{"TURN_ANY_INTO_EMPTY_STRING",
                       alwaysTrue,
                       [](TestRewriteContext& ctx) {
                           ctx.current() = "";
                           return false;
                       },
                       100}});
        return false;
    };

    RewriteEngine<TestRewriteContext> engine{{
        strings,
        {
            {"ENQUEUE_UPPERCASE_HELLO", isHello, enqueueHighestPriorityRule, 2},
        },
    }};

    engine.applyRules();

    ASSERT_EQ(strings.size(), 2U);
    ASSERT_EQ(strings[0], "");
    ASSERT_EQ(strings[1], "world");
}

TEST(RuleBasedRewriterTest, RunRulesThatHaveSpecificTags) {
    enum TestRuleTags : TagSet {
        Foo = 1 << 0,
        Bar = 1 << 1,
        Baz = 1 << 2,
    };

    auto makeRule = [](std::string name, TagSet tags) {
        return Rule<TestRewriteContext>{
            std::move(name), alwaysTrue, appendExclamationTransform, 1, tags};
    };

    std::vector<std::string> strings = {{"hello"}};
    RewriteEngine<TestRewriteContext> engine{
        {strings,
         {
             makeRule("SHOULD_APPLY_1", TestRuleTags::Foo),
             makeRule("SHOULD_APPLY_2", TestRuleTags::Bar),
             makeRule("SHOULD_APPLY_3", TestRuleTags::Bar | TestRuleTags::Baz),
             {"SHOULD_NOT_APPLY", alwaysTrue, shouldNeverRun, 1, TestRuleTags::Baz},
         }}};

    auto tagsToRun = TestRuleTags::Foo | TestRuleTags::Bar;
    engine.applyRules(tagsToRun);

    ASSERT_EQ(strings.size(), 1U);
    ASSERT_EQ(strings[0], "hello!!!");
}

DEATH_TEST(RuleBasedRewriterTestDeathTest, EnqueueRuleFromRuleThenRequeue, "11010015") {
    std::vector<std::string> strings = {{"1", "2", "3"}};

    RewriteEngine<TestRewriteContext> engine{{
        strings,
        {
            {"ENQUEUE_RULE_THEN_REQUEUE",
             alwaysTrue,
             [&](TestRewriteContext& ctx) {
                 // This rule should never fire because rules should be cleared when we requeue.
                 ctx.addRules({{"SHOULD_NOT_RUN", alwaysTrue, shouldNeverRun, 100}});
                 return true;
             },
             1},
        },
    }};

    engine.applyRules();
}

}  // namespace
}  // namespace mongo::rule_based_rewrites
