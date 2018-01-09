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

#include "mongo/db/update/set_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using SetNodeTest = UpdateNodeTest;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

DEATH_TEST(SetNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$set: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    node.init(update["$set"].embeddedObject().firstElement(), expCtx).transitional_ignore();
}

TEST(SetNodeTest, InitSucceedsForNonemptyElement) {
    auto update = fromjson("{$set: {a: 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));
}

TEST_F(SetNodeTest, ApplyNoOp) {
    auto update = fromjson("{$set: {a: 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyEmptyPathToCreate) {
    auto update = fromjson("{$set: {a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 6}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 6}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCreatePath) {
    auto update = fromjson("{$set: {'a.b.c': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {d: 5}}"));
    setPathToCreate("b.c");
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {d: 5, b: {c: 6}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.b.c': 6}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCreatePathFromRoot) {
    auto update = fromjson("{$set: {'a.b': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{c: 5}"));
    setPathToCreate("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{c: 5, a: {b: 6}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.b': 6}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyPositional) {
    auto update = fromjson("{$set: {'a.$': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.$"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2]}"));
    setPathTaken("a.1");
    setMatchedField("1");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][1]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 6, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.1': 6}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyNonViablePathToCreate) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathToCreate("b");
    setPathTaken("a");
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()["a"])),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {a: 5}");
}

TEST_F(SetNodeTest, ApplyNonViablePathToCreateFromReplicationIsNoOp) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathToCreate("b");
    setPathTaken("a");
    addIndexedPath("a");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$set: {a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken("a");
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 6}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyDoesNotAffectIndexes) {
    auto update = fromjson("{$set: {a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken("a");
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 6}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, TypeChangeIsNotANoop) {
    auto update = fromjson("{$set: {a: NumberLong(2)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: NumberInt(2)}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberLong(2)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, IdentityOpOnDeserializedIsNotANoOp) {
    // Apply an op that would be a no-op.
    auto update = fromjson("{$set: {a: {b : NumberInt(2)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: { b: NumberInt(0)}}"));
    // Apply a mutation to the document that will make it non-serialized.
    doc.root()["a"]["b"].setValueInt(2).transitional_ignore();

    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b : NumberInt(2)}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyEmptyDocument) {
    auto update = fromjson("{$set: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyInPlace) {
    auto update = fromjson("{$set: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyOverridePath) {
    auto update = fromjson("{$set: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyChangeType) {
    auto update = fromjson("{$set: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 'str'}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyNewPath) {
    auto update = fromjson("{$set: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{b: 1}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1, a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyLog) {
    auto update = fromjson("{$set: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyNoOpDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b : 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, TypeChangeOnDottedPathIsNotANoOp) {
    auto update = fromjson("{$set: {'a.b': NumberInt(2)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: NumberLong(2)}}"));
    setPathTaken("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b : NumberLong(2)}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyPathNotViable) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a:1}"));
    setPathToCreate("b");
    setPathTaken("a");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()["a"])),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {a: 1}");
}

TEST_F(SetNodeTest, ApplyPathNotViableArrray) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a:[{b:1}]}"));
    setPathToCreate("b");
    setPathTaken("a");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()["a"])),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {a: [ { b: 1 } ]}");
}

TEST_F(SetNodeTest, ApplyInPlaceDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathTaken("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyChangeTypeDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 'str'}}"));
    setPathTaken("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyChangePath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 1}}}"));
    setPathTaken("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyExtendPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {c: 1}}"));
    setPathToCreate("b");
    setPathTaken("a");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {c: 1, b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyNewDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{c: 1}"));
    setPathToCreate("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{c: 1, a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyEmptyDoc) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyFieldWithDot) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{'a.b':4}"));
    setPathToCreate("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{'a.b':4, a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyNoOpArrayIndex) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"));
    setPathTaken("a.2.b");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"][2]["b"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, TypeChangeInArrayIsNotANoOp) {
    auto update = fromjson("{$set: {'a.2.b': NumberInt(2)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 2.0}]}"));
    setPathTaken("a.2.b");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"][2]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: NumberInt(2)}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyNonViablePath) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()["a"])),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field '2' in element {a: 0}");
}

TEST_F(SetNodeTest, ApplyInPlaceArrayIndex) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 1}]}"));
    setPathTaken("a.2.b");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"][2]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyNormalArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0},{b: 1}]}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyPaddingArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},null,{b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyNumericObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 0}}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 0, '2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyNumericField) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {'2': {b: 1}}}"));
    setPathTaken("a.2.b");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["2"]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyExtendNumericField) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {'2': {c: 1}}}"));
    setPathToCreate("b");
    setPathTaken("a.2");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["2"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {c: 1, b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyEmptyObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {}}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyEmptyArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    addIndexedPath("a.2.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [null, null, {b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyLogDottedPath) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b:0}, {b:1}]}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_EQUALS(fromjson("{a: [{b:0}, {b:1}, {b:2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, LogEmptyArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_EQUALS(fromjson("{a: [null, null, {b:2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, LogEmptyObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {}}"));
    setPathToCreate("2.b");
    setPathTaken("a");
    node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyNoOpComplex) {
    auto update = fromjson("{$set: {'a.1.b': {c: 1, d: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, d: 1}}]}}"));
    setPathTaken("a.1.b");
    addIndexedPath("a.1.b");
    auto result = node.apply(getApplyParams(doc.root()["a"][1]["b"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, d: 1}}]}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplySameStructure) {
    auto update = fromjson("{$set: {'a.1.b': {c: 1, d: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, xxx: 1}}]}}"));
    setPathTaken("a.1.b");
    addIndexedPath("a.1.b");
    auto result = node.apply(getApplyParams(doc.root()["a"][1]["b"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, d: 1}}]}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, NonViablePathWithoutRepl) {
    auto update = fromjson("{$set: {'a.1.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathToCreate("1.b");
    setPathTaken("a");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()["a"])),
                                AssertionException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field '1' in element {a: 1}");
}

TEST_F(SetNodeTest, SingleFieldFromReplication) {
    auto update = fromjson("{$set: {'a.1.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], expCtx));

    mutablebson::Document doc(fromjson("{_id:1, a: 1}"));
    setPathToCreate("1.b");
    setPathTaken("a");
    addIndexedPath("a.1.b");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id:1, a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, SingleFieldNoIdFromReplication) {
    auto update = fromjson("{$set: {'a.1.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathToCreate("1.b");
    setPathTaken("a");
    addIndexedPath("a.1.b");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, NestedFieldFromReplication) {
    auto update = fromjson("{$set: {'a.a.1.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.a.1.b"], expCtx));

    mutablebson::Document doc(fromjson("{_id:1, a: {a: 1}}"));
    setPathToCreate("1.b");
    setPathTaken("a.a");
    addIndexedPath("a.a.1.b");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"]["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id:1, a: {a: 1}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, DoubleNestedFieldFromReplication) {
    auto update = fromjson("{$set: {'a.b.c.d': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c.d"], expCtx));

    mutablebson::Document doc(fromjson("{_id:1, a: {b: {c: 1}}}"));
    setPathToCreate("d");
    setPathTaken("a.b.c");
    addIndexedPath("a.b.c.d");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]["c"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id:1, a: {b: {c: 1}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, NestedFieldNoIdFromReplication) {
    auto update = fromjson("{$set: {'a.a.1.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.a.1.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {a: 1}}"));
    setPathToCreate("1.b");
    setPathTaken("a.a");
    addIndexedPath("a.a.1.b");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"]["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {a: 1}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ReplayArrayFieldNotAppendedIntermediateFromReplication) {
    auto update = fromjson("{$set: {'a.0.b': [0,2]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.0.b"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 0, a: [1, {b: [1]}]}"));
    setPathToCreate("b");
    setPathTaken("a.0");
    addIndexedPath("a.1.b");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"][0]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0, a: [1, {b: [1]}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, Set6) {
    auto update = fromjson("{$set: {'r.a': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["r.a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 1, r: {a:1, b:2}}"));
    setPathTaken("r.a");
    addIndexedPath("r.a");
    auto result = node.apply(getApplyParams(doc.root()["r"]["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 1, r: {a:2, b:2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'r.a': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, Set6FromRepl) {
    auto update = fromjson("{$set: { 'r.a': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["r.a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 1, r: {a:1, b:2}}"));
    setPathTaken("r.a");
    addIndexedPath("r.a");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["r"]["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 1, r: {a:2, b:2} }"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'r.a': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplySetModToEphemeralDocument) {
    // The following mod when applied to a document constructed node by node exposed a
    // latent debug only defect in mutable BSON, so this is more a test of mutable than
    // $set.
    auto update = fromjson("{ $set: { x: { a: 100, b: 2 }}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["x"], expCtx));

    mutablebson::Document doc;
    Element x = doc.makeElementObject("x");
    doc.root().pushBack(x).transitional_ignore();
    Element a = doc.makeElementInt("a", 100);
    x.pushBack(a).transitional_ignore();

    setPathTaken("x");
    addIndexedPath("x");
    auto result = node.apply(getApplyParams(doc.root()["x"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{ x : { a : 100, b : 2 } }"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplyCannotCreateDollarPrefixedFieldInsideSetElement) {
    auto update = fromjson("{$set: {a: {$bad: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"])),
        AssertionException,
        ErrorCodes::DollarPrefixedFieldName,
        "The dollar ($) prefixed field '$bad' in 'a.$bad' is not valid for storage.");
}

TEST_F(SetNodeTest, ApplyCannotCreateDollarPrefixedFieldAtStartOfPath) {
    auto update = fromjson("{$set: {'$bad.a': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["$bad.a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("$bad.a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root())),
        AssertionException,
        ErrorCodes::DollarPrefixedFieldName,
        "The dollar ($) prefixed field '$bad' in '$bad' is not valid for storage.");
}

TEST_F(SetNodeTest, ApplyCannotCreateDollarPrefixedFieldInMiddleOfPath) {
    auto update = fromjson("{$set: {'a.$bad.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.$bad.b"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a.$bad.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root())),
        AssertionException,
        ErrorCodes::DollarPrefixedFieldName,
        "The dollar ($) prefixed field '$bad' in 'a.$bad' is not valid for storage.");
}

TEST_F(SetNodeTest, ApplyCannotCreateDollarPrefixedFieldAtEndOfPath) {
    auto update = fromjson("{$set: {'a.$bad': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.$bad"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a.$bad");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root())),
        AssertionException,
        ErrorCodes::DollarPrefixedFieldName,
        "The dollar ($) prefixed field '$bad' in 'a.$bad' is not valid for storage.");
}

TEST_F(SetNodeTest, ApplyCanCreateDollarPrefixedFieldNameWhenValidateForStorageIsFalse) {
    auto update = fromjson("{$set: {$bad: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["$bad"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("$bad");
    addIndexedPath("$bad");
    setValidateForStorage(false);
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{$bad: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {$bad: 1}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCannotOverwriteImmutablePath) {
    auto update = fromjson("{$set: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a.b");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]["b"])),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b' would modify the immutable field 'a.b'");
}

TEST_F(SetNodeTest, ApplyCanPerformNoopOnImmutablePath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a.b");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 0u);
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCannotOverwritePrefixToRemoveImmutablePath) {
    auto update = fromjson("{$set: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"])),
        AssertionException,
        ErrorCodes::ImmutableField,
        "After applying the update, the immutable field 'a.b' was found to have been removed.");
}

TEST_F(SetNodeTest, ApplyCannotOverwritePrefixToModifyImmutablePath) {
    auto update = fromjson("{$set: {a: {b: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()["a"])),
                                AssertionException,
                                ErrorCodes::ImmutableField,
                                "After applying the update, the immutable field 'a.b' was found to "
                                "have been altered to b: 1");
}

TEST_F(SetNodeTest, ApplyCanPerformNoopOnPrefixOfImmutablePath) {
    auto update = fromjson("{$set: {a: {b: 2}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 0u);
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCanOverwritePrefixToCreateImmutablePath) {
    auto update = fromjson("{$set: {a: {b: 2}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: {b: 2}}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCanOverwritePrefixOfImmutablePathIfNoopOnImmutablePath) {
    auto update = fromjson("{$set: {a: {b: 2, c: 3}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2, c: 3}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: {b: 2, c: 3}}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCannotOverwriteSuffixOfImmutablePath) {
    auto update = fromjson("{$set: {'a.b.c': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 2}}}"));
    setPathTaken("a.b.c");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]["b"]["c"])),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b.c' would modify the immutable field 'a.b'");
}

TEST_F(SetNodeTest, ApplyCanPerformNoopOnSuffixOfImmutablePath) {
    auto update = fromjson("{$set: {'a.b.c': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 2}}}"));
    setPathTaken("a.b.c");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]["c"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: {c: 2}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 0u);
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCannotCreateFieldAtEndOfImmutablePath) {
    auto update = fromjson("{$set: {'a.b.c': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {}}}"));
    setPathToCreate("c");
    setPathTaken("a.b");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]["b"])),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Updating the path 'a.b' to b: { c: 1 } would modify the immutable field 'a.b'");
}

TEST_F(SetNodeTest, ApplyCannotCreateFieldBeyondEndOfImmutablePath) {
    auto update = fromjson("{$set: {'a.b.c': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {}}}"));
    setPathToCreate("c");
    setPathTaken("a.b");
    addImmutablePath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]["b"])),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Updating the path 'a.b' to b: { c: 1 } would modify the immutable field 'a'");
}

TEST_F(SetNodeTest, ApplyCanCreateImmutablePath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {}}"));
    setPathToCreate("b");
    setPathTaken("a");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.b': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplyCanCreatePrefixOfImmutablePath) {
    auto update = fromjson("{$set: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplySetFieldInNonExistentArrayElementAffectsIndexOnSiblingField) {
    auto update = fromjson("{$set: {'a.1.c': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    setPathToCreate("1.c");
    setPathTaken("a");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0}, {c: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.1.c': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplySetFieldInExistingArrayElementDoesNotAffectIndexOnSiblingField) {
    auto update = fromjson("{$set: {'a.0.c': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.0.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    setPathToCreate("c");
    setPathTaken("a.0");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"][0]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0, c: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.0.c': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplySetFieldInNonExistentNumericFieldDoesNotAffectIndexOnSiblingField) {
    auto update = fromjson("{$set: {'a.1.c': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {'0': {b: 0}}}"));
    setPathToCreate("1.c");
    setPathTaken("a");
    addIndexedPath("a.b");
    addIndexedPath("a.1.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'0': {b: 0}, '1': {c: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(getLogDoc().root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.1.c': 2}}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplySetOnInsertIsNoopWhenInsertIsFalse) {
    auto update = fromjson("{$setOnInsert: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node(UpdateNode::Context::kInsertOnly);
    ASSERT_OK(node.init(update["$setOnInsert"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(SetNodeTest, ApplySetOnInsertIsAppliedWhenInsertIsTrue) {
    auto update = fromjson("{$setOnInsert: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node(UpdateNode::Context::kInsertOnly);
    ASSERT_OK(node.init(update["$setOnInsert"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    setInsert(true);
    addIndexedPath("a");
    setLogBuilderToNull();  // The log builder is null for inserts.
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(SetNodeTest, ApplySetOnInsertExistingPath) {
    auto update = fromjson("{$setOnInsert: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    SetNode node(UpdateNode::Context::kInsertOnly);
    ASSERT_OK(node.init(update["$setOnInsert"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    setInsert(true);
    addIndexedPath("a");
    setLogBuilderToNull();  // The log builder is null for inserts.
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

}  // namespace
}  // namespace mongo
