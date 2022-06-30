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

#include "mongo/db/update/update_object_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/update/conflict_placeholder_node.h"
#include "mongo/db/update/rename_node.h"
#include "mongo/db/update/update_array_node.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using UpdateObjectNodeTest = UpdateTestFixture;
using unittest::assertGet;

TEST(UpdateObjectNodeTest, InvalidPathFailsToParse) {
    auto update = fromjson("{$set: {'': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"][""],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::EmptyFieldName);
    ASSERT_EQ(result.getStatus().reason(), "An empty update path is not valid.");
}

TEST(UpdateObjectNodeTest, ValidIncPathParsesSuccessfully) {
    auto update = fromjson("{$inc: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_INC,
                                              update["$inc"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidMulPathParsesSuccessfully) {
    auto update = fromjson("{$mul: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_MUL,
                                              update["$mul"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidRenamePathParsesSuccessfully) {
    auto update = fromjson("{$rename: {'a.b': 'c.d'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_RENAME,
                                              update["$rename"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    // There should be a ConflictPlaceHolderNode along the "a.b" path.
    auto aChild = dynamic_cast<UpdateObjectNode*>(root.getChild("a"));
    ASSERT(aChild);

    auto bChild = dynamic_cast<ConflictPlaceholderNode*>(aChild->getChild("b"));
    ASSERT(bChild);

    // There should be a RenameNode along the "c.d" path.
    auto cChild = dynamic_cast<UpdateObjectNode*>(root.getChild("c"));
    ASSERT(cChild);

    auto dChild = dynamic_cast<RenameNode*>(cChild->getChild("d"));
    ASSERT(dChild);
}

TEST(UpdateObjectNodeTest, ValidSetPathParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidUnsetPathParsesSuccessfully) {
    auto update = fromjson("{$unset: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_UNSET,
                                              update["$unset"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidAddToSetPathParsesSuccessfully) {
    auto update = fromjson("{$addToSet: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_ADD_TO_SET,
                                              update["$addToSet"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidPopPathParsesSuccessfully) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_POP,
                                              update["$pop"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidMaxPathParsesSuccessfully) {
    auto update = fromjson("{$max: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_MAX,
                                              update["$max"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidMinPathParsesSuccessfully) {
    auto update = fromjson("{$min: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_MIN,
                                              update["$min"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidCurrentDatePathParsesSuccessfully) {
    auto update = fromjson("{$currentDate: {'a.b': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_CURRENTDATE,
                                              update["$currentDate"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidSetOnInsertPathParsesSuccessfully) {
    auto update = fromjson("{$setOnInsert: {'a.b': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET_ON_INSERT,
                                              update["$setOnInsert"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ValidPushParsesSuccessfully) {
    auto update = fromjson("{$push: {'a.b': {$each: [0, 1], $sort: 1, $position: 0, $slice: 10}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_PUSH,
                                              update["$push"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, MultiplePositionalElementsFailToParse) {
    auto update = fromjson("{$set: {'a.$.b.$': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$.b.$"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "Too many positional (i.e. '$') elements found in path 'a.$.b.$'");
}

TEST(UpdateObjectNodeTest, ParsingSetsPositionalTrue) {
    auto update = fromjson("{$set: {'a.$.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$.b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_OK(result);
    ASSERT_TRUE(result.getValue());
}

TEST(UpdateObjectNodeTest, ParsingSetsPositionalFalse) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_OK(result);
    ASSERT_FALSE(result.getValue());
}

TEST(UpdateObjectNodeTest, PositionalElementFirstPositionFailsToParse) {
    auto update = fromjson("{$set: {'$': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["$"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "Cannot have positional (i.e. '$') element in the first position in path '$'");
}

TEST(UpdateObjectNodeTest, TwoModifiersOnSameFieldFailToParse) {
    auto update = fromjson("{$set: {a: 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(), "Updating the path 'a' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, TwoModifiersOnDifferentFieldsParseSuccessfully) {
    auto update = fromjson("{$set: {a: 5, b: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, TwoModifiersWithSameDottedPathFailToParse) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b' would create a conflict at 'a.b'");
}

TEST(UpdateObjectNodeTest, FirstModifierPrefixOfSecondFailToParse) {
    auto update = fromjson("{$set: {a: 5, 'a.b': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, FirstModifierDottedPrefixOfSecondFailsToParse) {
    auto update = fromjson("{$set: {'a.b': 5, 'a.b.c': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.b.c"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b.c' would create a conflict at 'a.b'");
}

TEST(UpdateObjectNodeTest, SecondModifierPrefixOfFirstFailsToParse) {
    auto update = fromjson("{$set: {'a.b': 5, a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(), "Updating the path 'a' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, SecondModifierDottedPrefixOfFirstFailsToParse) {
    auto update = fromjson("{$set: {'a.b.c': 5, 'a.b': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b' would create a conflict at 'a.b'");
}

TEST(UpdateObjectNodeTest, ModifiersWithCommonPrefixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.b': 5, 'a.c': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ModifiersWithCommonDottedPrefixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.b.c': 5, 'a.b.d': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, ModifiersWithCommonPrefixDottedSuffixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.b.c': 5, 'a.d.e': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.d.e"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, TwoModifiersOnSamePositionalFieldFailToParse) {
    auto update = fromjson("{$set: {'a.$': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$' would create a conflict at 'a.$'");
}

TEST(UpdateObjectNodeTest, PositionalFieldsWithDifferentPrefixesParseSuccessfully) {
    auto update = fromjson("{$set: {'a.$': 5, 'b.$': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["b.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, PositionalAndNonpositionalFieldWithCommonPrefixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.$': 5, 'a.0': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.0"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, TwoModifiersWithSamePositionalDottedPathFailToParse) {
    auto update = fromjson("{$set: {'a.$.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$.b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$.b' would create a conflict at 'a.$.b'");
}

TEST(UpdateObjectNodeTest, FirstModifierPositionalPrefixOfSecondFailsToParse) {
    auto update = fromjson("{$set: {'a.$': 5, 'a.$.b': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$.b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$.b' would create a conflict at 'a.$'");
}

TEST(UpdateObjectNodeTest, SecondModifierPositionalPrefixOfFirstFailsToParse) {
    auto update = fromjson("{$set: {'a.$.b': 5, 'a.$': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$' would create a conflict at 'a.$'");
}

TEST(UpdateObjectNodeTest, FirstModifierFieldPrefixOfSecondParsesSuccessfully) {
    auto update = fromjson("{$set: {'a': 5, 'ab': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["ab"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, SecondModifierFieldPrefixOfSecondParsesSuccessfully) {
    auto update = fromjson("{$set: {'ab': 5, 'a': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["ab"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
}

TEST(UpdateObjectNodeTest, IdentifierWithoutArrayFilterFailsToParse) {
    auto update = fromjson("{$set: {'a.$[i]': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$[i]"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "No array filter found for identifier 'i' in path 'a.$[i]'");
}

TEST(UpdateObjectNodeTest, IdentifierInMiddleOfPathWithoutArrayFilterFailsToParse) {
    auto update = fromjson("{$set: {'a.$[i].b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$[i].b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "No array filter found for identifier 'i' in path 'a.$[i].b'");
}

TEST(UpdateObjectNodeTest, EmptyIdentifierParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.$[]': 5}}");
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
    ASSERT_TRUE(foundIdentifiers.empty());
}

TEST(UpdateObjectNodeTest, EmptyIdentifierInMiddleOfPathParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.$[].b': 5}}");
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
    ASSERT_TRUE(foundIdentifiers.empty());
}

TEST(UpdateObjectNodeTest, IdentifierWithArrayFilterParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.$[i]': 5}}");
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
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
}

TEST(UpdateObjectNodeTest, IdentifierWithArrayFilterInMiddleOfPathParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.$[i].b': 5}}");
    auto arrayFilter = fromjson("{i: 0}");
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
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
}

TEST(UpdateObjectNodeTest, IdentifierInFirstPositionFailsToParse) {
    auto update = fromjson("{$set: {'$[i]': 5}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["$[i]"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "Cannot have array filter identifier (i.e. '$[<id>]') element in the first position "
              "in path '$[i]'");
}

TEST(UpdateObjectNodeTest, IdentifierInFirstPositionWithSuffixFailsToParse) {
    auto update = fromjson("{$set: {'$[i].a': 5}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["$[i].a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "Cannot have array filter identifier (i.e. '$[<id>]') element in the first position "
              "in path '$[i].a'");
}

TEST(UpdateObjectNodeTest, CreateObjectNodeInSamePositionAsArrayNodeFailsToParse) {
    auto update = fromjson("{$set: {'a.$[i]': 5, 'a.0': 6}}");
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
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.0"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.0' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, CreateArrayNodeInSamePositionAsObjectNodeFailsToParse) {
    auto update = fromjson("{$set: {'a.0': 5, 'a.$[i]': 6}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.0"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$[i]"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$[i]' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, CreateLeafNodeInSamePositionAsArrayNodeFailsToParse) {
    auto update = fromjson("{$set: {'a.$[i]': 5, a: 6}}");
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
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(), "Updating the path 'a' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, CreateArrayNodeInSamePositionAsLeafNodeFailsToParse) {
    auto update = fromjson("{$set: {a: 5, 'a.$[i]': 6}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$[i]"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$[i]' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, CreateTwoChildrenOfArrayNodeParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.$[i]': 5, 'a.$[j]': 6}}");
    auto arrayFilterI = fromjson("{i: 0}");
    auto arrayFilterJ = fromjson("{j: 0}");
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
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[j]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_EQ(foundIdentifiers.size(), 2U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    ASSERT_TRUE(foundIdentifiers.find("j") != foundIdentifiers.end());
}

TEST(UpdateObjectNodeTest, ConflictAtArrayNodeChildFailsToParse) {
    auto update1 = fromjson("{$set: {'a.$[i]': 5}}");
    auto update2 = fromjson("{$set: {'a.$[i]': 6}}");
    auto arrayFilter = fromjson("{i: 0}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update1["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update2["$set"]["a.$[i]"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$[i]' would create a conflict at 'a.$[i]'");
}

TEST(UpdateObjectNodeTest, ConflictThroughArrayNodeChildFailsToParse) {
    auto update = fromjson("{$set: {'a.$[i].b': 5, 'a.$[i].b.c': 6}}");
    auto arrayFilter = fromjson("{i: 0}");
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
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_SET,
                                                  update["$set"]["a.$[i].b.c"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$[i].b.c' would create a conflict at 'a.$[i].b'");
}

TEST(UpdateObjectNodeTest, NoConflictDueToDifferentArrayNodeChildrenParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.$[i].b': 5, 'a.$[j].b.c': 6}}");
    auto arrayFilterI = fromjson("{i: 0}");
    auto arrayFilterJ = fromjson("{j: 0}");
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
    ASSERT_EQ(foundIdentifiers.size(), 1U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.$[j].b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_EQ(foundIdentifiers.size(), 2U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    ASSERT_TRUE(foundIdentifiers.find("j") != foundIdentifiers.end());
}

TEST(UpdateObjectNodeTest, MultipleArrayNodesAlongPathParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.$[i].$[j].$[i]': 5}}");
    auto arrayFilterI = fromjson("{i: 0}");
    auto arrayFilterJ = fromjson("{j: 0}");
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
                                              update["$set"]["a.$[i].$[j].$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_EQ(foundIdentifiers.size(), 2U);
    ASSERT_TRUE(foundIdentifiers.find("i") != foundIdentifiers.end());
    ASSERT_TRUE(foundIdentifiers.find("j") != foundIdentifiers.end());
}

/**
 * Used to test if the fields in an input UpdateObjectNode match an expected set of fields.
 */
static bool fieldsMatch(const std::vector<std::string>& expectedFields,
                        const UpdateInternalNode& node) {
    for (const std::string& fieldName : expectedFields) {
        if (!node.getChild(fieldName)) {
            return false;
        }
    }

    // There are no expected fields that aren't in the UpdateInternalNode. There is no way to check
    // if UpdateInternalNodes contains any fields that are not in the expected set, because the
    // UpdateInternalNodes API does not expose its list of child fields in any way other than
    // getChild().
    return true;
}

TEST(UpdateObjectNodeTest, DistinctFieldsMergeCorrectly) {
    auto setUpdate1 = fromjson("{$set: {'a': 5}}");
    auto setUpdate2 = fromjson("{$set: {'ab': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["ab"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    auto result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
    ASSERT_TRUE(result);

    ASSERT_TRUE(result->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*result) == typeid(UpdateObjectNode&));
    auto mergedRootNode = static_cast<UpdateObjectNode*>(result.get());
    ASSERT_TRUE(fieldsMatch(std::vector<std::string>{"a", "ab"}, *mergedRootNode));
}

TEST(UpdateObjectNodeTest, NestedMergeSucceeds) {
    auto setUpdate1 = fromjson("{$set: {'a.c': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.d': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    auto result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
    ASSERT_TRUE(result);

    ASSERT_TRUE(result->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*result) == typeid(UpdateObjectNode&));
    auto mergedRootNode = static_cast<UpdateObjectNode*>(result.get());
    ASSERT_TRUE(fieldsMatch({"a"}, *mergedRootNode));

    ASSERT_TRUE(mergedRootNode->getChild("a"));
    ASSERT_TRUE(mergedRootNode->getChild("a")->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*mergedRootNode->getChild("a")) == typeid(UpdateObjectNode&));
    auto aNode = static_cast<UpdateObjectNode*>(mergedRootNode->getChild("a"));
    ASSERT_TRUE(fieldsMatch({"c", "d"}, *aNode));
}

TEST(UpdateObjectNodeTest, DoublyNestedMergeSucceeds) {
    auto setUpdate1 = fromjson("{$set: {'a.b.c': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.b.d': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.b.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    auto result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
    ASSERT_TRUE(result);

    ASSERT_TRUE(result->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*result) == typeid(UpdateObjectNode&));
    auto mergedRootNode = static_cast<UpdateObjectNode*>(result.get());
    ASSERT_TRUE(fieldsMatch({"a"}, *mergedRootNode));

    ASSERT_TRUE(mergedRootNode->getChild("a"));
    ASSERT_TRUE(mergedRootNode->getChild("a")->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*mergedRootNode->getChild("a")) == typeid(UpdateObjectNode&));
    auto aNode = static_cast<UpdateObjectNode*>(mergedRootNode->getChild("a"));
    ASSERT_TRUE(fieldsMatch({"b"}, *aNode));

    ASSERT_TRUE(aNode->getChild("b"));
    ASSERT_TRUE(aNode->getChild("b")->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*aNode->getChild("b")) == typeid(UpdateObjectNode&));
    auto bNode = static_cast<UpdateObjectNode*>(aNode->getChild("b"));
    ASSERT_TRUE(fieldsMatch({"c", "d"}, *bNode));
}

TEST(UpdateObjectNodeTest, FieldAndPositionalMergeCorrectly) {
    auto setUpdate1 = fromjson("{$set: {'a.b': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    auto result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
    ASSERT_TRUE(result);

    ASSERT_TRUE(result->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*result) == typeid(UpdateObjectNode&));
    auto mergedRootNode = static_cast<UpdateObjectNode*>(result.get());
    ASSERT_TRUE(fieldsMatch(std::vector<std::string>{"a"}, *mergedRootNode));

    ASSERT_TRUE(mergedRootNode->getChild("a"));
    ASSERT_TRUE(mergedRootNode->getChild("a")->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*mergedRootNode->getChild("a")) == typeid(UpdateObjectNode&));
    auto aNode = static_cast<UpdateObjectNode*>(mergedRootNode->getChild("a"));
    ASSERT_TRUE(aNode->getChild("$"));
    ASSERT_TRUE(fieldsMatch({"b"}, *aNode));
}

TEST(UpdateObjectNodeTest, MergeThroughPositionalSucceeds) {
    auto setUpdate1 = fromjson("{$set: {'a.$.b': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$.c': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    auto result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
    ASSERT_TRUE(result);

    ASSERT_TRUE(result->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*result) == typeid(UpdateObjectNode&));
    auto mergedRootNode = static_cast<UpdateObjectNode*>(result.get());
    ASSERT_TRUE(fieldsMatch({"a"}, *mergedRootNode));

    ASSERT_TRUE(mergedRootNode->getChild("a"));
    ASSERT_TRUE(mergedRootNode->getChild("a")->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*mergedRootNode->getChild("a")) == typeid(UpdateObjectNode&));
    auto aNode = static_cast<UpdateObjectNode*>(mergedRootNode->getChild("a"));
    ASSERT_TRUE(fieldsMatch({}, *aNode));

    ASSERT_TRUE(aNode->getChild("$"));
    ASSERT_TRUE(aNode->getChild("$")->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*aNode->getChild("$")) == typeid(UpdateObjectNode&));
    auto positionalNode = static_cast<UpdateObjectNode*>(aNode->getChild("$"));
    ASSERT_TRUE(fieldsMatch({"b", "c"}, *positionalNode));
}

TEST(UpdateObjectNodeTest, TopLevelConflictFails) {
    auto setUpdate1 = fromjson("{$set: {'a': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a'");
}

TEST(UpdateObjectNodeTest, NestedConflictFails) {
    auto setUpdate1 = fromjson("{$set: {'a.b': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.b': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a.b'");
}

TEST(UpdateObjectNodeTest, LeftPrefixMergeFails) {
    auto setUpdate1 = fromjson("{$set: {'a.b': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.b.c': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a.b'");
}

TEST(UpdateObjectNodeTest, RightPrefixMergeFails) {
    auto setUpdate1 = fromjson("{$set: {'a.b.c': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.b': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a.b'");
}

TEST(UpdateObjectNodeTest, LeftPrefixMergeThroughPositionalFails) {
    auto setUpdate1 = fromjson("{$set: {'a.$.c': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$.c.d': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$.c.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a.$.c'");
}

TEST(UpdateObjectNodeTest, RightPrefixMergeThroughPositionalFails) {
    auto setUpdate1 = fromjson("{$set: {'a.$.c.d': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$.c': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$.c.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a.$.c'");
}

TEST(UpdateObjectNodeTest, MergeWithConflictingPositionalFails) {
    auto setUpdate1 = fromjson("{$set: {'a.$': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a.$'");
}

DEATH_TEST_REGEX(UpdateObjectNodeTest,
                 MergingArrayNodesWithDifferentArrayFiltersFails,
                 "Invariant failure.*leftNode._arrayFilters == &rightNode._arrayFilters") {
    auto setUpdate1 = fromjson("{$set: {'a.$[i]': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$[j]': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto arrayFilterI = fromjson("{i: 0}");
    auto arrayFilterJ = fromjson("{j: 0}");
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters1;
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters2;
    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));
    arrayFilters1["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters2["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters1,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$[j]"],
                                              expCtx,
                                              arrayFilters2,
                                              foundIdentifiers));

    UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
}

TEST(UpdateObjectNodeTest, MergingArrayNodeWithObjectNodeFails) {
    auto setUpdate1 = fromjson("{$set: {'a.$[i]': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.b': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto arrayFilter = fromjson("{i: 0}");
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a'");
}

TEST(UpdateObjectNodeTest, MergingArrayNodeWithLeafNodeFails) {
    auto setUpdate1 = fromjson("{$set: {'a.$[i]': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto arrayFilter = fromjson("{i: 0}");
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a'");
}

TEST(UpdateObjectNodeTest, MergingTwoArrayNodesSucceeds) {
    auto setUpdate1 = fromjson("{$set: {'a.$[i]': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$[j]': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto arrayFilterI = fromjson("{i: 0}");
    auto arrayFilterJ = fromjson("{j: 0}");
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;

    auto parsedFilterI = assertGet(MatchExpressionParser::parse(arrayFilterI, expCtx));
    auto parsedFilterJ = assertGet(MatchExpressionParser::parse(arrayFilterJ, expCtx));

    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterI)));
    arrayFilters["j"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilterJ)));

    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$[i]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$[j]"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    auto result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
    ASSERT_TRUE(result);

    ASSERT_TRUE(result->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*result) == typeid(UpdateObjectNode&));
    auto mergedRootNode = static_cast<UpdateObjectNode*>(result.get());
    ASSERT_TRUE(fieldsMatch({"a"}, *mergedRootNode));

    ASSERT_TRUE(mergedRootNode->getChild("a"));
    ASSERT_TRUE(mergedRootNode->getChild("a")->type == UpdateNode::Type::Array);
    ASSERT_TRUE(typeid(*mergedRootNode->getChild("a")) == typeid(UpdateArrayNode&));
    auto aNode = static_cast<UpdateArrayNode*>(mergedRootNode->getChild("a"));
    ASSERT_TRUE(fieldsMatch({"i", "j"}, *aNode));
}

TEST(UpdateObjectNodeTest, MergeConflictThroughArrayNodesFails) {
    auto setUpdate1 = fromjson("{$set: {'a.$[i].b.c': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$[i].b': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto arrayFilter = fromjson("{i: 0}");
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$[i].b.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    std::unique_ptr<UpdateNode> result;
    ASSERT_THROWS_CODE_AND_WHAT(
        result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef),
        AssertionException,
        ErrorCodes::ConflictingUpdateOperators,
        "Update created a conflict at 'root.a.$[i].b'");
}

TEST(UpdateObjectNodeTest, NoMergeConflictThroughArrayNodesSucceeds) {
    auto setUpdate1 = fromjson("{$set: {'a.$[i].b': 5}}");
    auto setUpdate2 = fromjson("{$set: {'a.$[i].c': 6}}");
    FieldRef fakeFieldRef("root");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto arrayFilter = fromjson("{i: 0}");
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    auto parsedFilter = assertGet(MatchExpressionParser::parse(arrayFilter, expCtx));
    arrayFilters["i"] = assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode setRoot1, setRoot2;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot1,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate1["$set"]["a.$[i].b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&setRoot2,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate2["$set"]["a.$[i].c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    auto result = UpdateNode::createUpdateNodeByMerging(setRoot1, setRoot2, &fakeFieldRef);
    ASSERT_TRUE(result);

    ASSERT_TRUE(result->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*result) == typeid(UpdateObjectNode&));
    auto mergedRootNode = static_cast<UpdateObjectNode*>(result.get());
    ASSERT_TRUE(fieldsMatch({"a"}, *mergedRootNode));

    ASSERT_TRUE(mergedRootNode->getChild("a"));
    ASSERT_TRUE(mergedRootNode->getChild("a")->type == UpdateNode::Type::Array);
    ASSERT_TRUE(typeid(*mergedRootNode->getChild("a")) == typeid(UpdateArrayNode&));
    auto aNode = static_cast<UpdateArrayNode*>(mergedRootNode->getChild("a"));
    ASSERT_TRUE(fieldsMatch({"i"}, *aNode));

    ASSERT_TRUE(aNode->getChild("i"));
    ASSERT_TRUE(aNode->getChild("i")->type == UpdateNode::Type::Object);
    ASSERT_TRUE(typeid(*aNode->getChild("i")) == typeid(UpdateObjectNode&));
    auto iNode = static_cast<UpdateObjectNode*>(aNode->getChild("i"));
    ASSERT_TRUE(fieldsMatch({"b", "c"}, *iNode));
}

TEST_F(UpdateObjectNodeTest, ApplyCreateField) {
    auto setUpdate = fromjson("{$set: {b: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: 5}"));
    addIndexedPath("b");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 5, b: 6}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {b: 6}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{b}");
}

TEST_F(UpdateObjectNodeTest, ApplyExistingField) {
    auto setUpdate = fromjson("{$set: {a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: 5}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 6}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: 6}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UpdateObjectNodeTest, ApplyExistingAndNonexistingFields) {
    auto setUpdate = fromjson("{$set: {a: 5, b: 6, c: 7, d: 8}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: 0, c: 0}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: 5, c: 7, b: 6, d: 8}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: 5, c: 7}, i: {b: 6, d: 8}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b, c, d}");
}

TEST_F(UpdateObjectNodeTest, ApplyExistingNestedPaths) {
    auto setUpdate = fromjson("{$set: {'a.b': 6, 'a.c': 7, 'b.d': 8, 'b.e': 9}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b.e"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: {b: 5, c: 5}, b: {d: 5, e: 5}}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: {b: 6, c: 7}, b: {d: 8, e: 9}}"), doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: 6, c: 7}}, sb: {u: {d: 8, e: 9}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b, a.c, b.d, b.e}");
}

TEST_F(UpdateObjectNodeTest, ApplyCreateNestedPaths) {
    auto setUpdate = fromjson("{$set: {'a.b': 6, 'a.c': 7, 'b.d': 8, 'b.e': 9}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b.e"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{z: 0}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{z: 0, a: {b: 6, c: 7}, b: {d: 8, e: 9}}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: {b: 6, c: 7}, b: {d: 8, e: 9}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b, a.c, b.d, b.e}");
}

TEST_F(UpdateObjectNodeTest, ApplyCreateDeeplyNestedPaths) {
    auto setUpdate = fromjson("{$set: {'a.b.c.d': 6, 'a.b.c.e': 7, 'a.f': 8}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.b.c.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.b.c.e"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.f"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{z: 0}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{z: 0, a: {b: {c: {d: 6, e: 7}}, f: 8}}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: {b: {c: {d: 6, e: 7}}, f: 8}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b.c.d, a.b.c.e, a.f}");
}

TEST_F(UpdateObjectNodeTest, ChildrenShouldBeAppliedInAlphabeticalOrder) {
    auto setUpdate = fromjson("{$set: {a: 5, d: 6, c: 7, b: 8, z: 9}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["z"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{z: 0, a: 0}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{z: 9, a: 5, b: 8, c: 7, d: 6}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: 5, z: 9}, i: {b: 8, c: 7, d: 6}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b, c, d, z}");
}

TEST_F(UpdateObjectNodeTest, CollatorShouldNotAffectUpdateOrder) {
    auto setUpdate = fromjson("{$set: {abc: 5, cba: 6}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["abc"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["cba"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{}"));
    addIndexedPath("abc");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{abc: 5, cba: 6}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {abc: 5, cba: 6}}}"));
}

TEST_F(UpdateObjectNodeTest, ApplyNoop) {
    auto setUpdate = fromjson("{$set: {a: 5, b: 6, c: 7}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: 5, b: 6, c: 7}"));
    addIndexedPath("a");
    addIndexedPath("b");
    addIndexedPath("c");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_TRUE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: 5, b: 6, c: 7}"), doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a, b, c}");
}

TEST_F(UpdateObjectNodeTest, ApplySomeChildrenNoops) {
    auto setUpdate = fromjson("{$set: {a: 5, b: 6, c: 7}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: 5, b: 0, c: 7}"));
    addIndexedPath("a");
    addIndexedPath("b");
    addIndexedPath("c");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: 5, b: 6, c: 7}"), doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {b: 6}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b, c}");
}

TEST_F(UpdateObjectNodeTest, ApplyBlockingElement) {
    auto setUpdate = fromjson("{$set: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: 0}"));
    addIndexedPath("a");
    ASSERT_EQUALS(getModifiedPaths(), "{}");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {a: 0}");
}

TEST_F(UpdateObjectNodeTest, ApplyBlockingElementFromReplication) {
    auto setUpdate = fromjson("{$set: {'a.b': 5, b: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: 0}"));
    addIndexedPath("a");
    setFromOplogApplication(true);
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: 0, b: 6}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {b: 6}}}"));
}

TEST_F(UpdateObjectNodeTest, ApplyPositionalMissingMatchedField) {
    auto setUpdate = fromjson("{$set: {'a.$': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{}"));
    addIndexedPath("a");
    ASSERT_EQUALS(getModifiedPaths(), "{}");
    ASSERT_THROWS_CODE_AND_WHAT(
        root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "The positional operator did not find the match needed from the query.");
}

TEST_F(UpdateObjectNodeTest, ApplyMergePositionalChild) {
    auto setUpdate = fromjson("{$set: {'a.0.b': 5, 'a.$.c': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.0.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 0, c: 0}]}"));
    setMatchedField("0");
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: [{b: 5, c: 6}]}"), doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {u: {b: 5, c: 6}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0.b, a.0.c}");
}

TEST_F(UpdateObjectNodeTest, ApplyOrderMergedPositionalChild) {
    auto setUpdate = fromjson("{$set: {'a.2': 5, 'a.1.b': 6, 'a.0': 7, 'a.$.c': 8}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.2"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.1.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.0"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{}"));
    setMatchedField("1");
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: {'0': 7, '1': {b: 6, c: 8}, '2': 5}}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: {'0': 7, '1': {b: 6, c: 8}, '2': 5}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0, a.1.b, a.1.c, a.2}");
}

TEST_F(UpdateObjectNodeTest, ApplyMergeConflictWithPositionalChild) {
    auto setUpdate = fromjson("{$set: {'a.0': 5, 'a.$': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.0"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{}"));
    setMatchedField("0");
    addIndexedPath("a");
    ASSERT_EQUALS(getModifiedPaths(), "{}");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::ConflictingUpdateOperators,
                                "Update created a conflict at 'a.0'");
}

TEST_F(UpdateObjectNodeTest, ApplyDoNotMergePositionalChild) {
    auto setUpdate = fromjson("{$set: {'a.0': 5, 'a.2': 6, 'a.$': 7}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.0"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.2"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{}"));
    setMatchedField("1");
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: {'0': 5, '1': 7, '2': 6}}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: {'0': 5, '1': 7, '2': 6}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0, a.1, a.2}");
}

TEST_F(UpdateObjectNodeTest, ApplyPositionalChildLast) {
    auto setUpdate = fromjson("{$set: {'a.$': 5, 'a.0': 6, 'a.1': 7}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.0"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.1"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{}"));
    setMatchedField("2");
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: {'0': 6, '1': 7, '2': 5}}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: {'0': 6, '1': 7, '2': 5}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0, a.1, a.2}");
}

TEST_F(UpdateObjectNodeTest, ApplyUseStoredMergedPositional) {
    auto setUpdate = fromjson("{$set: {'a.0.b': 5, 'a.$.c': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.0.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 0, c: 0}]}"));
    setMatchedField("0");
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: [{b: 5, c: 6}]}"), doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {u: {b: 5, c: 6}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0.b, a.0.c}");

    mutablebson::Document doc2(fromjson("{a: [{b: 0, c: 0}]}"));
    resetApplyParams();
    setMatchedField("0");
    addIndexedPath("a");
    result = root.apply(getApplyParams(doc2.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: [{b: 5, c: 6}]}"), doc2.getObject());
    ASSERT_TRUE(doc2.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {u: {b: 5, c: 6}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0.b, a.0.c}");
}

TEST_F(UpdateObjectNodeTest, ApplyDoNotUseStoredMergedPositional) {
    auto setUpdate = fromjson("{$set: {'a.0.b': 5, 'a.$.c': 6, 'a.1.d': 7}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.0.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.$.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.1.d"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 0, c: 0}, {c: 0, d: 0}]}"));
    setMatchedField("0");
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: [{b: 5, c: 6}, {c: 0, d: 7}]}"), doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(
        fromjson("{$v: 2, diff: {sa: {a: true, s0: {u: {b: 5, c: 6}}, s1: {u: {d: 7}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0.b, a.0.c, a.1.d}");

    mutablebson::Document doc2(fromjson("{a: [{b: 0, c: 0}, {c: 0, d: 0}]}"));
    resetApplyParams();
    setMatchedField("1");
    addIndexedPath("a");
    result = root.apply(getApplyParams(doc2.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: [{b: 5, c: 0}, {c: 6, d: 7}]}"), doc2.getObject());
    ASSERT_TRUE(doc2.isInPlaceModeEnabled());

    assertOplogEntry(
        fromjson("{$v: 2, diff: {sa: {a: true, s0: {u: {b: 5}}, s1: {u: {c: 6, d: 7}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0.b, a.1.c, a.1.d}");
}

/**
 * The leading zero case is interesting, because if we try to look up an array element by the index
 * string, a leading zero will cause the lookup to fail. That is, even if 'element' is an array,
 * element["02"] will not find the element with subscript 2.
 */
TEST_F(UpdateObjectNodeTest, ApplyToArrayByIndexWithLeadingZero) {
    auto setUpdate = fromjson("{$set: {'a.02': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.02"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [0, 0, 0, 0, 0]}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: [0, 0, 2, 0, 0]}"), doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u2: 2}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.02}");
}

/**
 * This test mimics a failure we saw in SERVER-29762. The failure occurred when the 'a.10' update
   (which was applied first) padded the empty array to have 10 elements, but the new padding
   elements did not have field names to match their array indexes. As a result, the 'a.2' update
   failed.
 */
TEST_F(UpdateObjectNodeTest, ApplyMultipleArrayUpdates) {
    auto setUpdate = fromjson("{$set: {'a.2': 2, 'a.10': 10}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.2"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.10"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: []}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(
        fromjson("{a: [null, null, 2, null, null, null, null, null, null, null, 10]}"),
        doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u2: 2, u10: 10}}}"));
}

TEST_F(UpdateObjectNodeTest, ApplyMultipleUpdatesToDocumentInArray) {
    auto setUpdate = fromjson("{$set: {'a.2.b': 1, 'a.2.c': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.2.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.2.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: []}"));
    addIndexedPath("a");
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: [null, null, {b: 1, c: 1}]}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u2: {b: 1, c: 1}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UpdateObjectNodeTest, ApplyUpdateToNonViablePathInArray) {
    auto setUpdate = fromjson("{$set: {'a.b': 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              setUpdate["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: [{b: 1}, {b: 2}]}"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {a: [ { b: 1 }, { b: 2 } ]}");
}

TEST_F(UpdateObjectNodeTest, SetAndPopModifiersWithCommonPrefixApplySuccessfully) {
    auto update = fromjson("{$set: {'a.b': 5}, $pop: {'a.c': -1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a.b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_POP,
                                              update["$pop"]["a.c"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));

    mutablebson::Document doc(fromjson("{a: {b: 3, c: [1, 2, 3, 4]}}"));
    auto result = root.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_FALSE(result.noop);
    ASSERT_BSONOBJ_EQ(fromjson("{a: {b: 5, c: [2, 3, 4]}}"), doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: 5, c: [ 2, 3, 4 ]}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b, a.c}");
}

TEST(ParseRenameTest, RenameToStringWithEmbeddedNullFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;

    {
        const auto embeddedNull = "a\0b"_sd;
        auto update = BSON("$rename" << BSON("a.b" << embeddedNull));

        UpdateObjectNode root;
        auto result = UpdateObjectNode::parseAndMerge(&root,
                                                      modifiertable::ModifierType::MOD_RENAME,
                                                      update["$rename"]["a.b"],
                                                      expCtx,
                                                      arrayFilters,
                                                      foundIdentifiers);
        ASSERT_NOT_OK(result);
        ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    }

    {
        const auto singleNullByte = "\0"_sd;
        auto update = BSON("$rename" << BSON("a.b" << singleNullByte));

        UpdateObjectNode root;
        auto result = UpdateObjectNode::parseAndMerge(&root,
                                                      modifiertable::ModifierType::MOD_RENAME,
                                                      update["$rename"]["a.b"],
                                                      expCtx,
                                                      arrayFilters,
                                                      foundIdentifiers);
        ASSERT_NOT_OK(result);
        ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    }

    {
        const auto leadingNullByte = "\0bbbb"_sd;
        auto update = BSON("$rename" << BSON("a.b" << leadingNullByte));

        UpdateObjectNode root;
        auto result = UpdateObjectNode::parseAndMerge(&root,
                                                      modifiertable::ModifierType::MOD_RENAME,
                                                      update["$rename"]["a.b"],
                                                      expCtx,
                                                      arrayFilters,
                                                      foundIdentifiers);
        ASSERT_NOT_OK(result);
        ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    }

    {
        const auto trailingNullByte = "bbbb\0"_sd;
        auto update = BSON("$rename" << BSON("a.b" << trailingNullByte));

        UpdateObjectNode root;
        auto result = UpdateObjectNode::parseAndMerge(&root,
                                                      modifiertable::ModifierType::MOD_RENAME,
                                                      update["$rename"]["a.b"],
                                                      expCtx,
                                                      arrayFilters,
                                                      foundIdentifiers);
        ASSERT_NOT_OK(result);
        ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    }
}

TEST(ParseRenameTest, RenameToNonUpdatablePathFails) {
    auto update = fromjson("{$rename: {'a': 'b.'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::EmptyFieldName);
    ASSERT_EQ(result.getStatus().reason(),
              "The update path 'b.' contains an empty field name, which is not allowed.");
}

TEST(ParseRenameTest, RenameFromNonUpdatablePathFails) {
    auto update = fromjson("{$rename: {'.a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"][".a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::EmptyFieldName);
    ASSERT_EQ(result.getStatus().reason(),
              "The update path '.a' contains an empty field name, which is not allowed.");
}

TEST(ParseRenameTest, RenameToNonStringPathFails) {
    auto update = fromjson("{$rename: {'a': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(), "The 'to' field for $rename must be a string: a: 5");
}

/**
 * This test, RenameUpwardFails, and RenameDownwardFails mirror similar tests in
 * rename_node_test.cpp. They exist to make sure that UpdateObjectNode::parseAndMerge() does not
 * observe a conflict between the RenameNode and the dummy ConflictPlaceHolderNode (generating an
 * "Update created a conflict" error message that does not really apply to these cases) before it
 * observes the more specific errors in RenameNode::init().
 */
TEST(ParseRenameTest, RenameWithSameNameFails) {
    auto update = fromjson("{$rename: {'a': 'a'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "The source and target field for $rename must differ: a: \"a\"");
}

TEST(ParseRenameTest, RenameUpwardFails) {
    auto update = fromjson("{$rename: {'b.a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"]["b.a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "The source and target field for $rename must not be on the same path: b.a: \"b\"");
}

TEST(ParseRenameTest, RenameDownwardFails) {
    auto update = fromjson("{$rename: {'b': 'b.a'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"]["b"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "The source and target field for $rename must not be on the same path: b: \"b.a\"");
}

TEST(ParseRenameTest, ConflictWithRenameSourceFailsToParse) {
    auto update = fromjson("{$set: {a: 5}, $rename: {a: 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["a"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(), "Updating the path 'a' would create a conflict at 'a'");
}

TEST(ParseRenameTest, ConflictWithRenameDestinationFailsToParse) {
    auto update = fromjson("{$set: {b: 5}, $rename: {a: 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    std::set<std::string> foundIdentifiers;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(&root,
                                              modifiertable::ModifierType::MOD_SET,
                                              update["$set"]["b"],
                                              expCtx,
                                              arrayFilters,
                                              foundIdentifiers));
    auto result = UpdateObjectNode::parseAndMerge(&root,
                                                  modifiertable::ModifierType::MOD_RENAME,
                                                  update["$rename"]["a"],
                                                  expCtx,
                                                  arrayFilters,
                                                  foundIdentifiers);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(), "Updating the path 'b' would create a conflict at 'b'");
}

}  // namespace
}  // namespace mongo
