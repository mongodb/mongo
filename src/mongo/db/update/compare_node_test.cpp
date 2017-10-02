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

#include "mongo/db/update/compare_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using CompareNodeTest = UpdateNodeTest;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

DEATH_TEST(CompareNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$max: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    node.init(update["$max"].embeddedObject().firstElement(), expCtx).ignore();
}

TEST_F(CompareNodeTest, ApplyMaxSameNumber) {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMinSameNumber) {
    auto update = fromjson("{$min: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMaxNumberIsLess) {
    auto update = fromjson("{$max: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMinNumberIsMore) {
    auto update = fromjson("{$min: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMaxSameValInt) {
    auto update = BSON("$max" << BSON("a" << 1LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1.0}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMaxSameValIntZero) {
    auto update = BSON("$max" << BSON("a" << 0LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0.0}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMinSameValIntZero) {
    auto update = BSON("$min" << BSON("a" << 0LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0.0}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMissingFieldMinNumber) {
    auto update = fromjson("{$min: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 0}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyExistingNumberMinNumber) {
    auto update = fromjson("{$min: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 0}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMissingFieldMaxNumber) {
    auto update = fromjson("{$max: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 0}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyExistingNumberMaxNumber) {
    auto update = fromjson("{$max: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 2}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyExistingDateMaxDate) {
    auto update = fromjson("{$max: {a: {$date: 123123123}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {$date: 0}}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {$date: 123123123}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: {$date: 123123123}}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyExistingEmbeddedDocMaxDoc) {
    auto update = fromjson("{$max: {a: {b: 3}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 3}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: {b: 3}}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyExistingEmbeddedDocMaxNumber) {
    auto update = fromjson("{$max: {a: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMinRespectsCollation) {
    auto update = fromjson("{$min: {a: 'dba'}}");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 'cbc'}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 'dba'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 'dba'}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMinRespectsCollationFromSetCollator) {
    auto update = fromjson("{$min: {a: 'dba'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    const CollatorInterfaceMock reverseStringCollator(
        CollatorInterfaceMock::MockType::kReverseString);
    node.setCollator(&reverseStringCollator);

    mutablebson::Document doc(fromjson("{a: 'cbc'}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 'dba'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 'dba'}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyMaxRespectsCollationFromSetCollator) {
    auto update = fromjson("{$max: {a: 'abd'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    const CollatorInterfaceMock reverseStringCollator(
        CollatorInterfaceMock::MockType::kReverseString);
    node.setCollator(&reverseStringCollator);

    mutablebson::Document doc(fromjson("{a: 'cbc'}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 'abd'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 'abd'}}"), getLogDoc());
}

DEATH_TEST(CompareNodeTest, CannotSetCollatorIfCollatorIsNonNull, "Invariant failure !_collator") {
    auto update = fromjson("{$max: {a: 1}}");
    CollatorInterfaceMock caseInsensitiveCollator(CollatorInterfaceMock::MockType::kToLowerString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&caseInsensitiveCollator);
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));
    node.setCollator(&caseInsensitiveCollator);
}

DEATH_TEST(CompareNodeTest, CannotSetCollatorTwice, "Invariant failure !_collator") {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&caseInsensitiveCollator);
    node.setCollator(&caseInsensitiveCollator);
}

TEST_F(CompareNodeTest, ApplyIndexesNotAffected) {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 1}}"), getLogDoc());
}

TEST_F(CompareNodeTest, ApplyNoIndexDataOrLogBuilder) {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

}  // namespace
}  // namespace mongo
