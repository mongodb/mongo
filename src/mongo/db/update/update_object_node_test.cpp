/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/update_object_node.h"

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(UpdateObjectNodeTest, InvalidPathFailsToParse) {
    auto update = fromjson("{$set: {'': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"][""], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::EmptyFieldName);
    ASSERT_EQ(result.getStatus().reason(), "An empty update path is not valid.");
}

TEST(UpdateObjectNodeTest, ValidPathParsesSuccessfully) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator));
}

TEST(UpdateObjectNodeTest, MultiplePositionalElementsFailToParse) {
    auto update = fromjson("{$set: {'a.$.b.$': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$.b.$"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "Too many positional (i.e. '$') elements found in path 'a.$.b.$'");
}

TEST(UpdateObjectNodeTest, ParsingSetsPositionalTrue) {
    auto update = fromjson("{$set: {'a.$.b': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$.b"], collator);
    ASSERT_OK(result);
    ASSERT_TRUE(result.getValue());
}

TEST(UpdateObjectNodeTest, ParsingSetsPositionalFalse) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator);
    ASSERT_OK(result);
    ASSERT_FALSE(result.getValue());
}

TEST(UpdateObjectNodeTest, PositionalElementFirstPositionFailsToParse) {
    auto update = fromjson("{$set: {'$': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["$"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "Cannot have positional (i.e. '$') element in the first position in path '$'");
}

// TODO SERVER-28777: All modifier types should succeed.
TEST(UpdateObjectNodeTest, IncFailsToParse) {
    auto update = fromjson("{$inc: {a: 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_INC, update["$inc"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::FailedToParse);
    ASSERT_EQ(result.getStatus().reason(), "Cannot construct modifier of type 3");
}

TEST(UpdateObjectNodeTest, TwoModifiersOnSameFieldFailToParse) {
    auto update = fromjson("{$set: {a: 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(), "Updating the path 'a' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, TwoModifiersOnDifferentFieldsParseSuccessfully) {
    auto update = fromjson("{$set: {a: 5, b: 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["b"], collator));
}

TEST(UpdateObjectNodeTest, TwoModifiersWithSameDottedPathFailToParse) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b' would create a conflict at 'a.b'");
}

TEST(UpdateObjectNodeTest, FirstModifierPrefixOfSecondFailToParse) {
    auto update = fromjson("{$set: {a: 5, 'a.b': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, FirstModifierDottedPrefixOfSecondFailsToParse) {
    auto update = fromjson("{$set: {'a.b': 5, 'a.b.c': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b.c"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b.c' would create a conflict at 'a.b'");
}

TEST(UpdateObjectNodeTest, SecondModifierPrefixOfFirstFailsToParse) {
    auto update = fromjson("{$set: {'a.b': 5, a: 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(), "Updating the path 'a' would create a conflict at 'a'");
}

TEST(UpdateObjectNodeTest, SecondModifierDottedPrefixOfFirstFailsToParse) {
    auto update = fromjson("{$set: {'a.b.c': 5, 'a.b': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b.c"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.b' would create a conflict at 'a.b'");
}

TEST(UpdateObjectNodeTest, ModifiersWithCommonPrefixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.b': 5, 'a.c': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.c"], collator));
}

TEST(UpdateObjectNodeTest, ModifiersWithCommonDottedPrefixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.b.c': 5, 'a.b.d': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b.c"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b.d"], collator));
}

TEST(UpdateObjectNodeTest, ModifiersWithCommonPrefixDottedSuffixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.b.c': 5, 'a.d.e': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.b.c"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.d.e"], collator));
}

TEST(UpdateObjectNodeTest, TwoModifiersOnSamePositionalFieldFailToParse) {
    auto update = fromjson("{$set: {'a.$': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$' would create a conflict at 'a.$'");
}

TEST(UpdateObjectNodeTest, PositionalFieldsWithDifferentPrefixesParseSuccessfully) {
    auto update = fromjson("{$set: {'a.$': 5, 'b.$': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["b.$"], collator));
}

TEST(UpdateObjectNodeTest, PositionalAndNonpositionalFieldWithCommonPrefixParseSuccessfully) {
    auto update = fromjson("{$set: {'a.$': 5, 'a.0': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.0"], collator));
}

TEST(UpdateObjectNodeTest, TwoModifiersWithSamePositionalDottedPathFailToParse) {
    auto update = fromjson("{$set: {'a.$.b': 5}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$.b"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$.b"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$.b' would create a conflict at 'a.$.b'");
}

TEST(UpdateObjectNodeTest, FirstModifierPositionalPrefixOfSecondFailsToParse) {
    auto update = fromjson("{$set: {'a.$': 5, 'a.$.b': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$.b"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$.b' would create a conflict at 'a.$'");
}

TEST(UpdateObjectNodeTest, SecondModifierPositionalPrefixOfFirstFailsToParse) {
    auto update = fromjson("{$set: {'a.$.b': 5, 'a.$': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$.b"], collator));
    auto result = UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a.$"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ConflictingUpdateOperators);
    ASSERT_EQ(result.getStatus().reason(),
              "Updating the path 'a.$' would create a conflict at 'a.$'");
}

TEST(UpdateObjectNodeTest, FirstModifierFieldPrefixOfSecondParsesSuccessfully) {
    auto update = fromjson("{$set: {'a': 5, 'ab': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["ab"], collator));
}

TEST(UpdateObjectNodeTest, SecondModifierFieldPrefixOfSecondParsesSuccessfully) {
    auto update = fromjson("{$set: {'ab': 5, 'a': 6}}");
    const CollatorInterface* collator = nullptr;
    UpdateObjectNode root;
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["ab"], collator));
    ASSERT_OK(UpdateObjectNode::parseAndMerge(
        &root, modifiertable::ModifierType::MOD_SET, update["$set"]["a"], collator));
}

}  // namespace
