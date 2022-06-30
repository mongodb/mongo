/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/update/object_transform_executor.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using ObjectTransformExecutorTest = UpdateTestFixture;
using mongo::mutablebson::countChildren;
using mongo::mutablebson::Element;

TEST_F(ObjectTransformExecutorTest, Noop) {
    BSONObj input = fromjson("{a: 1, b: 2}");

    ObjectTransformExecutor node([&input](const BSONObj& pre) {
        ASSERT_BSONOBJ_BINARY_EQ(pre, input);
        return pre;
    });

    mutablebson::Document doc(input);
    auto result = node.applyUpdate(getApplyParams(doc.root()));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{}"), result.oplogEntry);
}

TEST_F(ObjectTransformExecutorTest, NoneNoop) {
    ObjectTransformExecutor node([](const BSONObj& pre) { return boost::none; });

    mutablebson::Document doc(fromjson("{a: 1, b: 2}"));
    auto result = node.applyUpdate(getApplyParams(doc.root()));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{}"), result.oplogEntry);
}

TEST_F(ObjectTransformExecutorTest, Replace) {
    BSONObj from = fromjson("{a: 1, b: 2}");
    BSONObj to = fromjson("{a: 1, b: 3}");

    ObjectTransformExecutor node([&from, &to](const BSONObj& pre) {
        ASSERT_BSONOBJ_BINARY_EQ(pre, from);
        return to;
    });

    mutablebson::Document doc(from);
    auto result = node.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(to, doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(to, result.oplogEntry);
}

TEST_F(ObjectTransformExecutorTest, ShouldSucceedWhenImmutableIdIsNotModified) {
    auto obj = fromjson("{_id: 0, a: 1, b: 2}");
    auto to = fromjson("{_id: 0, c: 1, d: 2}");
    ObjectTransformExecutor node([&obj, &to](const BSONObj& pre) {
        ASSERT_BSONOBJ_BINARY_EQ(pre, obj);
        return to;
    });

    mutablebson::Document doc(obj);
    addImmutablePath("_id");
    auto result = node.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(to, doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(to, result.oplogEntry);
}

TEST_F(ObjectTransformExecutorTest, CannotRemoveImmutablePath) {
    auto from = fromjson("{_id: 0, a: {b: 1}}");
    auto obj = fromjson("{_id: 0, c: 1}");
    ObjectTransformExecutor node([&from, &obj](const BSONObj& pre) {
        ASSERT_BSONOBJ_BINARY_EQ(pre, from);
        return obj;
    });

    mutablebson::Document doc(from);
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(node.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::ImmutableField,
                                "After applying the update, the 'a.b' (required and immutable) "
                                "field was found to have been removed --{ _id: 0, a: { b: 1 } }");
}

TEST_F(ObjectTransformExecutorTest, CannotReplaceImmutablePathWithArrayField) {
    auto obj = fromjson("{_id: 0, a: [{b: 1}]}");
    ObjectTransformExecutor node([&obj](const BSONObj& pre) { return obj; });

    mutablebson::Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(node.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::NotSingleValueField,
                                "After applying the update to the document, the (immutable) field "
                                "'a.b' was found to be an array or array descendant.");
}

TEST_F(ObjectTransformExecutorTest, CannotMakeImmutablePathArrayDescendant) {
    auto obj = fromjson("{_id: 0, a: [1]}");
    ObjectTransformExecutor node([&obj](const BSONObj& pre) { return obj; });

    mutablebson::Document doc(fromjson("{_id: 0, a: {'0': 1}}"));
    addImmutablePath("a.0");
    ASSERT_THROWS_CODE_AND_WHAT(node.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::NotSingleValueField,
                                "After applying the update to the document, the (immutable) field "
                                "'a.0' was found to be an array or array descendant.");
}

TEST_F(ObjectTransformExecutorTest, CannotModifyImmutablePath) {
    auto obj = fromjson("{_id: 0, a: {b: 2}}");
    ObjectTransformExecutor node([&obj](const BSONObj& pre) { return obj; });

    mutablebson::Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(node.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::ImmutableField,
                                "After applying the update, the (immutable) field 'a.b' was found "
                                "to have been altered to b: 2");
}

TEST_F(ObjectTransformExecutorTest, CannotModifyImmutableId) {
    auto obj = fromjson("{_id: 1}");
    ObjectTransformExecutor node([&obj](const BSONObj& pre) { return obj; });

    mutablebson::Document doc(fromjson("{_id: 0}"));
    addImmutablePath("_id");
    ASSERT_THROWS_CODE_AND_WHAT(node.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::ImmutableField,
                                "After applying the update, the (immutable) field '_id' was found "
                                "to have been altered to _id: 1");
}

TEST_F(ObjectTransformExecutorTest, CanAddImmutableField) {
    auto obj = fromjson("{a: {b: 1}}");
    ObjectTransformExecutor node([&obj](const BSONObj& pre) { return obj; });

    mutablebson::Document doc(fromjson("{c: 1}"));
    addImmutablePath("a.b");
    auto result = node.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 1}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{a: {b: 1}}"), result.oplogEntry);
}

TEST_F(ObjectTransformExecutorTest, CanAddImmutableId) {
    auto obj = fromjson("{_id: 0}");
    ObjectTransformExecutor node([&obj](const BSONObj& pre) { return obj; });

    mutablebson::Document doc(fromjson("{c: 1}"));
    addImmutablePath("_id");
    auto result = node.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{_id: 0}"), result.oplogEntry);
}

}  // namespace
}  // namespace mongo
