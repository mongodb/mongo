// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe::value {
namespace {

using MakeObjSpecHash = EExpressionTestFixture;
using FieldAction = MakeObjSpec::FieldAction;
using Keep = MakeObjSpec::Keep;
using Drop = MakeObjSpec::Drop;
using SetArg = MakeObjSpec::SetArg;
using LambdaArg = MakeObjSpec::LambdaArg;
using MakeObj = MakeObjSpec::MakeObj;
using NonObjInputBehavior = MakeObjSpec::NonObjInputBehavior;
using FieldListScope = MakeObjSpec::FieldListScope;

TEST(MakeObjSpecTest, GetActionOfAnUnlistedFieldDependsOnTheScope) {
    std::vector<FieldAction> actions;
    actions.emplace_back(SetArg{0});
    MakeObjSpec open{FieldListScope::kOpen, std::vector<std::string>{"a"}, std::move(actions)};
    ASSERT_TRUE(open.getAction(open.fields.findPos("a")).isSetArg());
    ASSERT_TRUE(open.getAction(open.fields.findPos("b")).isKeep());

    MakeObjSpec closed{FieldListScope::kClosed, std::vector<std::string>{"a"}};
    ASSERT_TRUE(closed.getAction(closed.fields.findPos("a")).isKeep());
    ASSERT_TRUE(closed.getAction(closed.fields.findPos("b")).isDrop());
}

TEST(MakeObjSpecTest, GetArgIdxReturnsTheIndexOfAnyArgumentAction) {
    ASSERT_EQ(FieldAction{SetArg{1}}.getArgIdx(), 1u);
    ASSERT_EQ(FieldAction{MakeObjSpec::AddArg{2}}.getArgIdx(), 2u);
    ASSERT_EQ((FieldAction{LambdaArg{3, false}}.getArgIdx()), 3u);
}

TEST(MakeObjSpecTest, ValidateDefaultActions) {
    for (auto scope : {FieldListScope::kOpen, FieldListScope::kClosed}) {
        MakeObjSpec empty{scope, {}};
        MakeObjSpec defaults{scope, {"a", "b"}};
        ASSERT_TRUE(empty.validate());
        ASSERT_TRUE(defaults.validate());
    }
}

TEST(MakeObjSpecTest, NonnegativeAndUnlimitedTraversalDepthsAreValid) {
    const boost::optional<int32_t> depths[] = {boost::none, 0, 2};
    for (auto depth : depths) {
        MakeObjSpec spec{FieldListScope::kOpen, {}, {}, NonObjInputBehavior::kNewObj, depth};
        ASSERT_TRUE(spec.validate());
    }
}

DEATH_TEST(MakeObjSpecDeathTest, NegativeTraversalDepth, "13948300") {
    MakeObjSpec spec{FieldListScope::kOpen, {}, {}, NonObjInputBehavior::kNewObj, -1};
    spec.validate();
}

TEST(MakeObjSpecTest, ValidateFlatSpecAndMetadata) {
    std::vector<FieldAction> actions;
    actions.emplace_back(SetArg{0});
    actions.emplace_back(MakeObjSpec::AddArg{0});
    actions.emplace_back(LambdaArg{2, false});
    actions.emplace_back(Drop{});
    MakeObjSpec valid{FieldListScope::kOpen, {"a", "b", "c", "d"}, std::move(actions)};
    // Shared indexes and unused argument positions are allowed.
    ASSERT_TRUE(valid.validate());

    auto invalidIndex = valid.clone();
    invalidIndex->actions[0] = SetArg{3};
    ASSERT_FALSE(invalidIndex->validate());

    auto missingAction = valid.clone();
    missingAction->actions.pop_back();
    ASSERT_FALSE(missingAction->validate());

    auto wrongArgCount = valid.clone();
    ++wrongArgCount->totalNumArgs;
    ASSERT_FALSE(wrongArgCount->validate());

    auto wrongSearchCount = valid.clone();
    ++wrongSearchCount->numFieldsToSearchFor;
    ASSERT_FALSE(wrongSearchCount->validate());

    auto wrongMandatoryOrder = valid.clone();
    std::swap(wrongMandatoryOrder->mandatoryFields[0], wrongMandatoryOrder->mandatoryFields[1]);
    ASSERT_FALSE(wrongMandatoryOrder->validate());

    auto missingMandatory = valid.clone();
    missingMandatory->mandatoryFields.pop_back();
    ASSERT_FALSE(missingMandatory->validate());

    auto invalidDisplay = valid.clone();
    invalidDisplay->displayOrder = {valid.fields.size()};
    ASSERT_FALSE(invalidDisplay->validate());
}

TEST(MakeObjSpecTest, ValidateNestedSpecsUseRootArgumentIndexes) {
    std::vector<FieldAction> nestedActions;
    nestedActions.emplace_back(SetArg{1});
    auto nested = std::make_unique<MakeObjSpec>(
        FieldListScope::kOpen, std::vector<std::string>{"value"}, std::move(nestedActions));
    auto& nestedAction = nested->actions.front();
    std::vector<FieldAction> actions;
    actions.emplace_back(MakeObj{std::move(nested)});
    actions.emplace_back(SetArg{0});
    MakeObjSpec root{FieldListScope::kClosed, {"nested", "root"}, std::move(actions)};

    // The nested spec has one argument-bearing action, but reads index 1 in the root call.
    ASSERT_TRUE(root.validate());

    nestedAction = SetArg{root.totalNumArgs};
    ASSERT_FALSE(root.validate());
}

std::vector<FieldAction> makeFieldActionVector(int depth = 2) {
    std::vector<FieldAction> fi;
    fi.emplace_back(Keep{});
    fi.emplace_back(Drop{});
    fi.emplace_back(SetArg{0});
    fi.emplace_back(SetArg{1});
    fi.emplace_back(LambdaArg{0, false});
    fi.emplace_back(LambdaArg{0, true});
    fi.emplace_back(
        MakeObj{std::make_unique<MakeObjSpec>(FieldListScope::kOpen, std::vector<std::string>{})});
    fi.emplace_back(MakeObj{
        std::make_unique<MakeObjSpec>(FieldListScope::kClosed, std::vector<std::string>{})});
    fi.emplace_back(MakeObj{std::make_unique<MakeObjSpec>(FieldListScope::kOpen,
                                                          std::vector<std::string>{},
                                                          std::vector<FieldAction>{},
                                                          NonObjInputBehavior::kReturnInput)});
    fi.emplace_back(MakeObj{std::make_unique<MakeObjSpec>(FieldListScope::kOpen,
                                                          std::vector<std::string>{},
                                                          std::vector<FieldAction>{},
                                                          NonObjInputBehavior::kReturnInput,
                                                          3)});
    // Tests recursive case!
    if (depth > 0) {
        auto fiv = makeFieldActionVector(depth - 1);
        std::vector<std::string> fieldNames;
        fieldNames.reserve(fiv.size());
        for (size_t i = 0; i < fiv.size(); i++) {
            fieldNames.push_back((std::stringstream() << "a" << i).str());
        }
        fi.emplace_back(MakeObj{std::make_unique<MakeObjSpec>(
            FieldListScope::kOpen, std::move(fieldNames), std::move(fiv))});
    }
    return fi;
}

std::vector<MakeObjSpec> makeMakeObjSpecVector() {
    std::vector<MakeObjSpec> mos;
    mos.emplace_back(FieldListScope::kOpen, std::vector<std::string>{});
    mos.emplace_back(FieldListScope::kClosed, std::vector<std::string>{});
    mos.emplace_back(FieldListScope::kClosed,
                     std::vector<std::string>{},
                     std::vector<FieldAction>{},
                     NonObjInputBehavior::kReturnNothing);
    mos.emplace_back(FieldListScope::kClosed,
                     std::vector<std::string>{},
                     std::vector<FieldAction>{},
                     NonObjInputBehavior::kReturnNothing,
                     3);

    for (size_t depth = 0; depth < 3; depth++) {
        auto fiv = makeFieldActionVector(depth);
        std::vector<std::string> fieldNames;
        fieldNames.reserve(fiv.size());
        for (size_t i = 0; i < fiv.size(); i++) {
            fieldNames.push_back((std::stringstream() << "a" << i).str());
        }
        mos.emplace_back(FieldListScope::kClosed, std::move(fieldNames), std::move(fiv));
    }

    return mos;
}

TEST_F(MakeObjSpecHash, TestFieldActionHash) {
    auto left = makeFieldActionVector();
    auto right = makeFieldActionVector();
    for (size_t i = 0; i < left.size(); i++) {
        ASSERT_EQ(left[i], right[i]);
        auto leftHash = absl::Hash<FieldAction>{}(left[i]);
        auto rightHash = absl::Hash<FieldAction>{}(right[i]);
        ASSERT_EQ(leftHash, rightHash);
        for (size_t j = i + 1; j < right.size(); j++) {
            ASSERT_NE(leftHash, absl::Hash<FieldAction>{}(right[j]));
        }
    }
}

TEST_F(MakeObjSpecHash, TestMakeObjSpecHash) {
    auto left = makeMakeObjSpecVector();
    auto right = makeMakeObjSpecVector();
    for (size_t i = 0; i < left.size(); i++) {
        ASSERT_EQ(left[i], right[i]);
        auto leftHash = absl::Hash<MakeObjSpec>{}(left[i]);
        auto rightHash = absl::Hash<MakeObjSpec>{}(right[i]);
        ASSERT_EQ(leftHash, rightHash);
        for (size_t j = i + 1; j < right.size(); j++) {
            ASSERT_NE(leftHash, absl::Hash<MakeObjSpec>{}(right[j]));
        }
    }
}

}  // namespace
}  // namespace mongo::sbe::value
