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

#include "mongo/db/update/current_date_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using CurrentDateNodeTest = UpdateNodeTest;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

DEATH_TEST(CurrentDateNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$currentDate: {}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    node.init(update["$currentDate"].embeddedObject().firstElement(), collator).ignore();
}

TEST(CurrentDateNodeTest, InitWithNonBoolNonObjectFails) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: 0}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithTrueSucceeds) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: true}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithFalseSucceeds) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: false}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithoutTypeFails) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: {}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithNonStringTypeFails) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: {$type: 1}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithBadValueTypeFails) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: {$type: 'bad'}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithTypeDateSucceeds) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: {$type: 'date'}}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithTypeTimestampSucceeds) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: {$type: 'timestamp'}}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithExtraFieldBeforeFails) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: {$bad: 1, $type: 'date'}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST(CurrentDateNodeTest, InitWithExtraFieldAfterFails) {
    const CollatorInterface* collator = nullptr;
    auto update = fromjson("{$currentDate: {a: {$type: 'date', $bad: 1}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], collator));
}

TEST_F(CurrentDateNodeTest, ApplyTrue) {
    auto update = fromjson("{$currentDate: {a: true}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    ASSERT_EQUALS(getLogDoc().root().countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"].countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"]["a"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"]["a"].getType(), BSONType::Date);
}

TEST_F(CurrentDateNodeTest, ApplyFalse) {
    auto update = fromjson("{$currentDate: {a: false}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    ASSERT_EQUALS(getLogDoc().root().countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"].countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"]["a"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"]["a"].getType(), BSONType::Date);
}

TEST_F(CurrentDateNodeTest, ApplyDate) {
    auto update = fromjson("{$currentDate: {a: {$type: 'date'}}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    ASSERT_EQUALS(getLogDoc().root().countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"].countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"]["a"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"]["a"].getType(), BSONType::Date);
}

TEST_F(CurrentDateNodeTest, ApplyTimestamp) {
    auto update = fromjson("{$currentDate: {a: {$type: 'timestamp'}}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::bsonTimestamp);

    ASSERT_EQUALS(getLogDoc().root().countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"].countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"]["a"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"]["a"].getType(), BSONType::bsonTimestamp);
}

TEST_F(CurrentDateNodeTest, ApplyFieldDoesNotExist) {
    auto update = fromjson("{$currentDate: {a: true}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    ASSERT_EQUALS(getLogDoc().root().countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"].countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"]["a"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"]["a"].getType(), BSONType::Date);
}

TEST_F(CurrentDateNodeTest, ApplyIndexesNotAffected) {
    auto update = fromjson("{$currentDate: {a: true}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    ASSERT_EQUALS(getLogDoc().root().countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"].countChildren(), 1U);
    ASSERT_TRUE(getLogDoc().root()["$set"]["a"].ok());
    ASSERT_EQUALS(getLogDoc().root()["$set"]["a"].getType(), BSONType::Date);
}

TEST_F(CurrentDateNodeTest, ApplyNoIndexDataOrLogBuilder) {
    auto update = fromjson("{$currentDate: {a: true}}");
    const CollatorInterface* collator = nullptr;
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], collator));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken("a");
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);
}

}  // namespace
}  // namespace
