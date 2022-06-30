/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/update/update_array_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/db/update/update_object_node.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using UpdateArrayNodeTest = UpdateTestFixture;
using unittest::assertGet;

TEST_F(UpdateArrayNodeTest, ApplyCreatePathFails) {
    auto update = fromjson("{$set: {'a.b.$[i]': 0}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: {}}"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "The path 'a.b' must exist in the document in order to apply array updates.");
}

TEST_F(UpdateArrayNodeTest, ApplyToNonArrayFails) {
    auto update = fromjson("{$set: {'a.$[i]': 0}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: {}}"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "Cannot apply array updates to non-array element a: {}");
}

TEST_F(UpdateArrayNodeTest, UpdateIsAppliedToAllMatchingElements) {
    auto update = fromjson("{$set: {'a.$[i]': 2}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [0, 1, 0]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [2, 1, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [2, 1, 2]}}}"));
    ASSERT_EQUALS("{a.0, a.2}", getModifiedPaths());
}

DEATH_TEST_REGEX_F(UpdateArrayNodeTest,
                   ArrayElementsMustNotBeDeserialized,
                   R"#(Invariant failure.*childElement.hasValue\(\))#") {
    auto update = fromjson("{$set: {'a.$[i].b': 0}}");
    auto arrayFilter = fromjson("{'i.c': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{c: 0}, {c: 0}, {c: 1}]}"));
    ASSERT_OK(doc.root()["a"][1]["c"].setValueInt(1));
    ASSERT_OK(doc.root()["a"][2]["c"].setValueInt(0));
    addIndexedPath("a");
    root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
}

TEST_F(UpdateArrayNodeTest, UpdateForEmptyIdentifierIsAppliedToAllArrayElements) {
    auto update = fromjson("{$set: {'a.$[]': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [0, 0, 0]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [1, 1, 1]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1, 1, 1]}}}"));
    ASSERT_EQUALS("{a.0, a.1, a.2}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyMultipleUpdatesToArrayElement) {
    auto update = fromjson("{$set: {'a.$[i].b': 1, 'a.$[j].c': 1, 'a.$[k].d': 1}}");
    auto arrayFilterI = fromjson("{'i.b': 0}");
    auto arrayFilterJ = fromjson("{'j.c': 0}");
    auto arrayFilterK = fromjson("{'k.d': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));
    auto parsedFilterK = assertGet(MatchExpressionParser::parse(arrayFilterK, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));
    arrayFilters["k"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterK)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[j].c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[k].d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 0, c: 0, d: 0}]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{b: 1, c: 1, d: 1}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {u: {b: 1, c: 1, d: 1}}}}}"));
    ASSERT_EQUALS("{a.0.b, a.0.c, a.0.d}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyMultipleUpdatesToArrayElementsUsingMergedChildrenCache) {
    auto update = fromjson("{$set: {'a.$[i].b': 1, 'a.$[j].c': 1}}");
    auto arrayFilterI = fromjson("{'i.b': 0}");
    auto arrayFilterJ = fromjson("{'j.c': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[j].c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 0, c: 0}, {b: 0, c: 0}]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{b: 1, c: 1}, {b: 1, c: 1}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [{b: 1, c: 1}, {b: 1, c: 1}]}}}"));
    ASSERT_EQUALS("{a.0.b, a.0.c, a.1.b, a.1.c}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyMultipleUpdatesToArrayElementsWithoutMergedChildrenCache) {
    auto update = fromjson("{$set: {'a.$[i].b': 2, 'a.$[j].c': 2, 'a.$[k].d': 2}}");
    auto arrayFilterI = fromjson("{'i.b': 0}");
    auto arrayFilterJ = fromjson("{'j.c': 0}");
    auto arrayFilterK = fromjson("{'k.d': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));
    auto parsedFilterK = assertGet(MatchExpressionParser::parse(arrayFilterK, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));
    arrayFilters["k"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterK)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[j].c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[k].d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 0, c: 0, d: 1}, {b: 1, c: 0, d: 0}]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{b: 2, c: 2, d: 1}, {b: 1, c: 2, d: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [{b: 2, c: 2, d: 1}, {b: 1, c: 2, d: 2}]}}}"));
    ASSERT_EQUALS("{a.0.b, a.0.c, a.1.c, a.1.d}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyMultipleUpdatesToArrayElementWithEmptyIdentifiers) {
    auto update = fromjson("{$set: {'a.$[].b': 1, 'a.$[].c': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[].c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 0, c: 0}]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{b: 1, c: 1}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {u: {b: 1, c: 1}}}}}"));
    ASSERT_EQUALS("{a.0.b, a.0.c}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyNestedArrayUpdates) {
    auto update = fromjson("{$set: {'a.$[i].b.$[j].c': 1, 'a.$[k].b.$[l].d': 1}}");
    auto arrayFilterI = fromjson("{'i.x': 0}");
    auto arrayFilterJ = fromjson("{'j.c': 0}");
    auto arrayFilterK = fromjson("{'k.x': 0}");
    auto arrayFilterL = fromjson("{'l.d': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));
    auto parsedFilterK = assertGet(MatchExpressionParser::parse(arrayFilterK, expCtx));
    auto parsedFilterL = assertGet(MatchExpressionParser::parse(arrayFilterL, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));
    arrayFilters["k"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterK)));
    arrayFilters["l"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterL)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b.$[j].c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[k].b.$[l].d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{x: 0, b: [{c: 0, d: 0}]}]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{x: 0, b: [{c: 1, d: 1}]}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(
        fromjson("{$v: 2, diff: {sa: {a: true, s0: {sb: {a: true, s0: {u: {c: 1, d: 1}}}}}}}"));
    ASSERT_EQUALS("{a.0.b.0.c, a.0.b.0.d}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyUpdatesWithMergeConflictToArrayElementFails) {
    auto update = fromjson("{$set: {'a.$[i]': 1, 'a.$[j]': 1}}");
    auto arrayFilterI = fromjson("{'i': 0}");
    auto arrayFilterJ = fromjson("{'j': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[j]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::ConflictingUpdateOperators,
                                "Update created a conflict at 'a.0'");
}

TEST_F(UpdateArrayNodeTest, ApplyUpdatesWithEmptyIdentifiersWithMergeConflictToArrayElementFails) {
    auto update = fromjson("{$set: {'a.$[].b.$[i]': 1, 'a.$[].b.$[j]': 1}}");
    auto arrayFilterI = fromjson("{'i': 0}");
    auto arrayFilterJ = fromjson("{'j': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[].b.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[].b.$[j]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: [0]}]}"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::ConflictingUpdateOperators,
                                "Update created a conflict at 'a.0.b.0'");
}

TEST_F(UpdateArrayNodeTest, ApplyNestedArrayUpdatesWithMergeConflictFails) {
    auto update = fromjson("{$set: {'a.$[i].b.$[j]': 1, 'a.$[k].b.$[l]': 1}}");
    auto arrayFilterI = fromjson("{'i.c': 0}");
    auto arrayFilterJ = fromjson("{j: 0}");
    auto arrayFilterK = fromjson("{'k.c': 0}");
    auto arrayFilterL = fromjson("{l: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));
    auto parsedFilterK = assertGet(MatchExpressionParser::parse(arrayFilterK, expCtx));
    auto parsedFilterL = assertGet(MatchExpressionParser::parse(arrayFilterL, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));
    arrayFilters["k"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterK)));
    arrayFilters["l"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterL)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b.$[j]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[k].b.$[l]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: [0], c: 0}]}"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::ConflictingUpdateOperators,
                                "Update created a conflict at 'a.0.b.0'");
}

TEST_F(UpdateArrayNodeTest, NoArrayElementsMatch) {
    auto update = fromjson("{$set: {'a.$[i]': 1}}");
    auto arrayFilter = fromjson("{'i': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [2, 2, 2]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_TRUE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [2, 2, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, UpdatesToAllArrayElementsAreNoops) {
    auto update = fromjson("{$set: {'a.$[i]': 1}}");
    auto arrayFilter = fromjson("{'i': 1}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [1, 1, 1]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_TRUE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [1, 1, 1]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a.0, a.1, a.2}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, NoArrayElementAffectsIndexes) {
    auto update = fromjson("{$set: {'a.$[i].b': 0}}");
    auto arrayFilter = fromjson("{'i.c': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{c: 0}, {c: 0}, {c: 0}]}"));
    addIndexedPath("a.c");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{c: 0, b: 0}, {c: 0, b: 0}, {c: 0, b: 0}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(
        fromjson("{$v: 2, diff: {u: {a: [{c: 0, b: 0}, {c: 0, b: 0}, {c: 0, b: 0}]}}}"));
    ASSERT_EQUALS("{a.0.b, a.1.b, a.2.b}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, WhenOneElementIsMatchedLogElementUpdateDirectly) {
    auto update = fromjson("{$set: {'a.$[i].b': 0}}");
    auto arrayFilter = fromjson("{'i.c': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{c: 1}, {c: 0}, {c: 1}]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{c: 1}, {c: 0, b: 0}, {c: 1}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s1: {i: {b: 0}}}}}"));
    ASSERT_EQUALS("{a.1.b}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, WhenOneElementIsModifiedLogElement) {
    auto update = fromjson("{$set: {'a.$[i].b': 0}}");
    auto arrayFilter = fromjson("{'i.c': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{c: 0, b: 0}, {c: 0}, {c: 1}]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{c: 0, b: 0}, {c: 0, b: 0}, {c: 1}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u1: {c: 0, b: 0}}}}"));
    ASSERT_EQUALS("{a.0.b, a.1.b}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ArrayUpdateOnEmptyArrayIsANoop) {
    auto update = fromjson("{$set: {'a.$[]': 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: []}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_TRUE(result.noop);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyPositionalInsideArrayUpdate) {
    auto update = fromjson("{$set: {'a.$[i].b.$': 1}}");
    auto arrayFilter = fromjson("{'i.c': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: [0, 0], c: 0}]}"));
    addIndexedPath("a");
    setMatchedField("1");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [{b: [0, 1], c: 0}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {sb: {a: true, u1: 1}}}}}"));
    ASSERT_EQUALS("{a.0.b.1}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyArrayUpdateFromReplication) {
    auto update = fromjson("{$set: {'a.$[i].b': 1}}");
    auto arrayFilter = fromjson("{'i': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    addIndexedPath("a");
    setFromOplogApplication(true);
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_TRUE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [0]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a.0.b}", getModifiedPaths());
}

TEST_F(UpdateArrayNodeTest, ApplyArrayUpdateNotFromReplication) {
    auto update = fromjson("{$set: {'a.$[i].b': 1}}");
    auto arrayFilter = fromjson("{'i': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {0: 0}");
}

TEST_F(UpdateArrayNodeTest, ApplyArrayUpdateWithoutLogBuilderOrIndexData) {
    auto update = fromjson("{$set: {'a.$[i]': 1}}");
    auto arrayFilter = fromjson("{'i': 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setLogBuilderToNull();
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS("{a.0}", getModifiedPaths());
}

}  // namespace
}  // namespace mongo
