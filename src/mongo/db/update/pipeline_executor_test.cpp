/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/update/pipeline_executor.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST_F(UpdateTestFixture, Noop) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1, b: 2}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{a: 1, b: 2}"));
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_TRUE(result.oplogEntry.isEmpty());
}

TEST_F(UpdateTestFixture, ShouldNotCreateIdIfNoIdExistsAndNoneIsSpecified) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1, b: 2}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{c: 1, d: 'largeStringValue'}"));
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{c: 1, d: 'largeStringValue', a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{$v: 2, diff: {i: {a: 1, b: 2}}}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, ShouldPreserveIdOfExistingDocumentIfIdNotReplaced) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1, b: 2}}"),
                                  fromjson("{$project: {a: 1, b: 1, _id: 0}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, c: 1, d: 2}"));
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{_id: 0, a: 1, b: 2}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, ShouldSucceedWhenImmutableIdIsNotModified) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 0, a: 1, b: 2}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, c: 1, d: 'largeStringValue'}"));
    addImmutablePath("_id");
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);

    ASSERT_EQUALS(fromjson("{_id: 0, c: 1, d: 'largeStringValue', a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{$v: 2, diff: {i: {a: 1, b: 2 }}}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, ComplexDoc) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1, b: [0, 1, 2], c: {d: 1}}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{a: 1, b: [0, 2, 2], e: ['val1', 'val2']}"));
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);

    ASSERT_EQUALS(fromjson("{a: 1, b: [0, 1, 2], e: ['val1', 'val2'], c: {d: 1}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{$v: 2, diff: {i: {c: {d: 1}}, sb: {a: true, u1: 1} }}"),
                             result.oplogEntry);
}

TEST_F(UpdateTestFixture, CannotRemoveImmutablePath) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$project: {c: 1}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(exec.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::ImmutableField,
                                "After applying the update, the 'a.b' (required and immutable) "
                                "field was found to have been removed --{ _id: 0, a: { b: 1 } }");
}


TEST_F(UpdateTestFixture, IdFieldIsNotRemoved) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$project: {a: 1, _id: 0}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, b: 1}"));
    addImmutablePath("_id");
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{_id: 0}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, CannotReplaceImmutablePathWithArrayField) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 0, a: [{b: 1}]}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(exec.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::NotSingleValueField,
                                "After applying the update to the document, the (immutable) field "
                                "'a.b' was found to be an array or array descendant.");
}

TEST_F(UpdateTestFixture, CannotMakeImmutablePathArrayDescendant) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 0, a: [1]}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, a: {'0': 1}}"));
    addImmutablePath("a.0");
    ASSERT_THROWS_CODE_AND_WHAT(exec.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::NotSingleValueField,
                                "After applying the update to the document, the (immutable) field "
                                "'a.0' was found to be an array or array descendant.");
}

TEST_F(UpdateTestFixture, CannotModifyImmutablePath) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 0, a: {b: 2}}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(exec.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::ImmutableField,
                                "After applying the update, the (immutable) field 'a.b' was found "
                                "to have been altered to b: 2");
}

TEST_F(UpdateTestFixture, CannotModifyImmutableId) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 1}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0}"));
    addImmutablePath("_id");
    ASSERT_THROWS_CODE_AND_WHAT(exec.applyUpdate(getApplyParams(doc.root())),
                                AssertionException,
                                ErrorCodes::ImmutableField,
                                "After applying the update, the (immutable) field '_id' was found "
                                "to have been altered to _id: 1");
}

TEST_F(UpdateTestFixture, CanAddImmutableField) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: {b: 1}}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{c: 1}"));
    addImmutablePath("a.b");
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{c: 1, a: {b: 1}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{c: 1, a: {b: 1}}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, CanAddImmutableId) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 0}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{c: 1}"));
    addImmutablePath("_id");
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{c: 1, _id: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{c: 1, _id: 0}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, CannotCreateDollarPrefixedName) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {'a.$bad': 1}}")};
    ASSERT_THROWS_CODE(PipelineExecutor(expCtx, pipeline), AssertionException, 16410);
}

TEST_F(UpdateTestFixture, NoLogBuilder) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{b: 1}"));
    setLogBuilderToNull();
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1, a: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(UpdateTestFixture, SerializeTest) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 0, a: [{b: 1}]}}"),
                                  fromjson("{$project: {a: 1}}"),
                                  fromjson("{$replaceWith: '$foo'}")};
    PipelineExecutor exec(expCtx, pipeline);

    auto serialized = exec.serialize();
    BSONObj doc(
        fromjson("[{$addFields: {_id: {$const: 0}, a: [{b: {$const: 1}}]}}, {$project: { _id: "
                 "true, a: true}}, {$replaceRoot: {newRoot: '$foo'}}]"));
    ASSERT_VALUE_EQ(serialized, Value(BSONArray(doc)));
}

TEST_F(UpdateTestFixture, RejectsInvalidConstantNames) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    const std::vector<BSONObj> pipeline;

    // Empty name.
    auto constants = BSON("" << 10);
    ASSERT_THROWS_CODE(PipelineExecutor(expCtx, pipeline, constants),
                       AssertionException,
                       ErrorCodes::FailedToParse);

    // Invalid first character.
    constants = BSON("^invalidFirstChar" << 10);
    ASSERT_THROWS_CODE(PipelineExecutor(expCtx, pipeline, constants),
                       AssertionException,
                       ErrorCodes::FailedToParse);

    // Contains invalid character.
    constants = BSON("contains*InvalidChar" << 10);
    ASSERT_THROWS_CODE(PipelineExecutor(expCtx, pipeline, constants),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(UpdateTestFixture, CanUseConstants) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    const std::vector<BSONObj> pipeline{fromjson("{$set: {b: '$$var1', c: '$$var2'}}")};
    const auto constants = BSON("var1" << 10 << "var2" << BSON("x" << 1 << "y" << 2));
    PipelineExecutor exec(expCtx, pipeline, constants);

    mutablebson::Document doc(fromjson("{a: 1}"));
    const auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 10, c : {x: 1, y: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{a: 1, b: 10, c : {x: 1, y: 2}}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, CanUseConstantsAcrossMultipleUpdates) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    const std::vector<BSONObj> pipeline{fromjson("{$set: {b: '$$var1'}}")};
    const auto constants = BSON("var1"
                                << "foo");
    PipelineExecutor exec(expCtx, pipeline, constants);

    // Update first doc.
    mutablebson::Document doc1(fromjson("{a: 1}"));
    auto result = exec.applyUpdate(getApplyParams(doc1.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 'foo'}"), doc1);
    ASSERT_FALSE(doc1.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{a: 1, b: 'foo'}"), result.oplogEntry);

    // Update second doc.
    mutablebson::Document doc2(fromjson("{a: 2}"));
    resetApplyParams();
    result = exec.applyUpdate(getApplyParams(doc2.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2, b: 'foo'}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{a: 2, b: 'foo'}"), result.oplogEntry);
}

TEST_F(UpdateTestFixture, NoopWithConstants) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    const std::vector<BSONObj> pipeline{fromjson("{$set: {a: '$$var1', b: '$$var2'}}")};
    const auto constants = BSON("var1" << 1 << "var2" << 2);
    PipelineExecutor exec(expCtx, pipeline, constants);

    mutablebson::Document doc(fromjson("{a: 1, b: 2}"));
    const auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_TRUE(result.oplogEntry.isEmpty());
}

TEST_F(UpdateTestFixture, TestIndexesAffectedWithDeletes) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj preImage(
        fromjson("{f1: {a: {b: {c: 1, paddingField: 'largeValueString'}, c: 1, paddingField: "
                 "'largeValueString'}}, paddingField: 'largeValueString'}"));
    addIndexedPath("p.a.b");
    addIndexedPath("f1.a.b");
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$unset: ['f1', 'f2', 'f3']}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(doc, fromjson("{paddingField: 'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry, fromjson("{$v: 2, diff: {d: {f1: false}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$unset: ['f1.a.p', 'f1.a.c', 'f1.a.b']}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(
            doc,
            fromjson(
                "{f1: {a: {paddingField: 'largeValueString'}}, paddingField: 'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {sf1: {sa: {d: {b: false, c: false}}}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$unset: 'f1.a.b.c'}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {sf1: {sa: {sb: {d: {c: false}}}}}}"));
        ASSERT_EQUALS(
            doc,
            fromjson("{f1: {a: {b: {paddingField: 'largeValueString'}, c: 1, paddingField: "
                     "'largeValueString'}}, paddingField: 'largeValueString'}"));
        ASSERT(result.indexesAffected);
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$unset: 'f1.a.c'}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {sf1: {sa: {d: {c: false}}}}}"));
        ASSERT_EQUALS(
            doc,
            fromjson("{f1: {a: {b: {c: 1, paddingField: 'largeValueString'}, paddingField: "
                     "'largeValueString'}}, paddingField: 'largeValueString'}"));
        ASSERT(!result.indexesAffected);
    }
}

TEST_F(UpdateTestFixture, TestIndexesAffectedWithUpdatesAndInserts) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj preImage(
        fromjson("{f1: {a: {b: {c: 1, paddingField: 'largeValueString'}, c: 1, paddingField: "
                 "'largeValueString'}}, paddingField: 'largeValueString'}"));
    addIndexedPath("p.a.b");
    addIndexedPath("f1.a.b");
    addIndexedPath("f1.a.newField");
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$set: {f1: true, f2: true}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(doc, fromjson("{f1: true, paddingField: 'largeValueString', f2: true}"));
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {u: {f1: true}, i: {f2: true}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$set: {'f1.a.newField': true}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify diff format.
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {sf1: {sa: {i: {newField: true}}}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$set: {'f1.a.b.c': true}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {sf1: {sa: {sb: {u: {c: true}}}}}}"));
        ASSERT_EQUALS(
            doc,
            fromjson(
                "{f1: {a: {b: {c: true, paddingField: 'largeValueString'}, c: 1, paddingField: "
                "'largeValueString'}}, paddingField: 'largeValueString'}"));
        ASSERT(result.indexesAffected);
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{fromjson("{$set: {'f1.a.p': true}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {sf1: {sa: {i: {p: true}}}}}"));
        ASSERT_EQUALS(
            doc,
            fromjson("{f1: {a: {b: {c: 1, paddingField: 'largeValueString'}, c: 1, paddingField: "
                     "'largeValueString', p: true}}, paddingField: 'largeValueString'}"));
        ASSERT(!result.indexesAffected);
    }
}

TEST_F(UpdateTestFixture, TestIndexesAffectedWithArraysAlongIndexPath) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj preImage(
        fromjson("{f1: [0, {a: {b: ['someStringValue', {c: 1, paddingField: 'largeValueString'}], "
                 "c: 1, paddingField: 'largeValueString'}}], paddingField: 'largeValueString'}"));
    addIndexedPath("p.a.b");
    addIndexedPath("f1.a.2.b");

    {
        // Test resize.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{
            fromjson("{$replaceWith: {f1: [0, {a: {b: ['someStringValue'], c: 1, paddingField: "
                     "'largeValueString'}}], paddingField: 'largeValueString'}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(
            doc,
            fromjson(
                "{f1: [0, {a: {b: ['someStringValue'], c: 1, paddingField: 'largeValueString'}}], "
                "paddingField: 'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(
            result.oplogEntry,
            fromjson("{$v: 2, diff: {sf1: {a: true, s1: {sa: {sb: {a: true, l: 1}}}}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When the index path is a prefix of a path in the diff and also involves numeric
        // components along the way. The numeric components should always be ignored.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{
            fromjson("{$replaceWith: {f1: [0, {a: {b: ['someStringValue', {c: 1, "
                     "paddingField:'largeValueString',d: 1}], c: 1, paddingField: "
                     "'largeValueString'}}], paddingField: 'largeValueString'}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(doc,
                      fromjson("{f1: [0, {a: {b: ['someStringValue', {c: 1, paddingField: "
                               "'largeValueString', d: 1}], c: 1, paddingField: "
                               "'largeValueString'}}], paddingField: 'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(
            result.oplogEntry,
            fromjson(
                "{$v: 2, diff: {sf1: {a: true, s1: {sa: {sb: {a: true, s1: {i: {d: 1} }}}}}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When inserting a sub-object into array, and the sub-object diverges from the index path.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{
            fromjson("{$set: {f1: {$concatArrays: ['$f1', [{newField: 1}]]}}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(
            doc,
            fromjson(
                "{f1: [0, {a: {b: ['someStringValue', {c: 1, paddingField: 'largeValueString'}], "
                "c: 1, paddingField: 'largeValueString'}}, {newField: 1}], paddingField: "
                "'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(result.oplogEntry,
                                 fromjson("{$v: 2, diff: {sf1: {a: true, u2: {newField: 1} }}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When a common array path element is updated, but the paths diverge at the last element.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{
            fromjson("{$replaceWith: {f1: [0, {a: {b: ['someStringValue', {c: 1, paddingField: "
                     "'largeValueString'}], c: 2, paddingField: 'largeValueString'}}], "
                     "paddingField: 'largeValueString'}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_BSONOBJ_BINARY_EQ(
            result.oplogEntry, fromjson("{$v: 2, diff: {sf1: {a: true, s1: {sa: {u: {c: 2} }}}}}"));
        ASSERT_EQUALS(
            doc,
            fromjson(
                "{f1: [0, {a: {b: ['someStringValue', {c: 1, paddingField: 'largeValueString'}], "
                "c: 2, paddingField: 'largeValueString'}}], paddingField: 'largeValueString'}"));
        ASSERT(!result.indexesAffected);
    }
}

TEST_F(UpdateTestFixture, TestIndexesAffectedWithArraysAfterIndexPath) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj preImage(
        fromjson("{f1: {a: {b: {c: [{paddingField: 'largeValueString'}, 1]}, c: 1, paddingField: "
                 "'largeValueString'}}, paddingField: 'largeValueString'}"));
    addIndexedPath("p.a.b");
    addIndexedPath("f1.a.2.b");

    {
        // Test resize.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{
            fromjson("{$set: {'f1.a.b.c': [{paddingField: 'largeValueString'}]}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(
            doc,
            fromjson("{f1: {a: {b: {c: [{paddingField: 'largeValueString'}]}, c: 1, paddingField: "
                     "'largeValueString'}}, paddingField: 'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(
            result.oplogEntry, fromjson("{$v: 2, diff: {sf1: {sa: {sb: {sc: {a: true, l: 1}}}}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // Add an array element.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{
            fromjson("{$set: {'f1.a.b.c': {$concatArrays: ['$f1.a.b.c', [{newField: 1}]]}}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(doc,
                      fromjson("{f1: {a: {b: {c: [{paddingField: 'largeValueString'}, 1, "
                               "{newField: 1}]}, c: 1, paddingField: 'largeValueString'}}, "
                               "paddingField: 'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(
            result.oplogEntry,
            fromjson("{$v: 2, diff: {sf1: {sa: {sb: {sc: {a: true, u2: {newField: 1} }}}}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // Updating a sub-array element.
        auto doc = mutablebson::Document(preImage);
        const std::vector<BSONObj> pipeline{
            fromjson("{$set: {'f1.a.b.c': [{paddingField: 'largeValueString'}, 'updatedVal']}}")};
        PipelineExecutor test(expCtx, pipeline);
        auto result = test.applyUpdate(getApplyParams(doc.root()));

        // Verify post-image and diff format.
        ASSERT_EQUALS(doc,
                      fromjson("{f1: {a: {b: {c: [{paddingField: 'largeValueString'}, "
                               "'updatedVal']}, c: 1, paddingField: 'largeValueString'}}, "
                               "paddingField: 'largeValueString'}"));
        ASSERT_BSONOBJ_BINARY_EQ(
            result.oplogEntry,
            fromjson("{$v: 2, diff: {sf1: {sa: {sb: {sc: {a: true, u1: 'updatedVal'}}}}}}"));
        ASSERT(result.indexesAffected);
    }
}

}  // namespace
}  // namespace mongo
