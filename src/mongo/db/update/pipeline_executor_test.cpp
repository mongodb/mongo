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
#include "mongo/db/json.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using PipelineExecutorTest = UpdateNodeTest;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

TEST_F(PipelineExecutorTest, Noop) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1, b: 2}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{a: 1, b: 2}"));
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, ShouldNotCreateIdIfNoIdExistsAndNoneIsSpecified) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1, b: 2}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{c: 1, d: 2}"));
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{c: 1, d: 2, a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{c: 1, d: 2, a: 1, b: 2}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, ShouldPreserveIdOfExistingDocumentIfIdNotReplaced) {
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
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1, b: 2}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, ShouldSucceedWhenImmutableIdIsNotModified) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {_id: 0, a: 1, b: 2}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{_id: 0, c: 1, d: 2}"));
    addImmutablePath("_id");
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0, c: 1, d: 2, a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{_id: 0, c: 1, d: 2, a: 1, b: 2}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, ComplexDoc) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {a: 1, b: [0, 1, 2], c: {d: 1}}}")};
    PipelineExecutor exec(expCtx, pipeline);

    mutablebson::Document doc(fromjson("{a: 1, b: [0, 2, 2], e: []}"));
    auto result = exec.applyUpdate(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: [0, 1, 2], e: [], c: {d: 1}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: 1, b: [0, 1, 2], e: [], c: {d: 1}}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, CannotRemoveImmutablePath) {
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


TEST_F(PipelineExecutorTest, IdFieldIsNotRemoved) {
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
    ASSERT_EQUALS(fromjson("{_id: 0}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, CannotReplaceImmutablePathWithArrayField) {
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

TEST_F(PipelineExecutorTest, CannotMakeImmutablePathArrayDescendant) {
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

TEST_F(PipelineExecutorTest, CannotModifyImmutablePath) {
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

TEST_F(PipelineExecutorTest, CannotModifyImmutableId) {
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

TEST_F(PipelineExecutorTest, CanAddImmutableField) {
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
    ASSERT_EQUALS(fromjson("{c: 1, a: {b: 1}}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, CanAddImmutableId) {
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
    ASSERT_EQUALS(fromjson("{c: 1, _id: 0}"), getLogDoc());
}

TEST_F(PipelineExecutorTest, CannotCreateDollarPrefixedName) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    std::vector<BSONObj> pipeline{fromjson("{$addFields: {'a.$bad': 1}}")};
    ASSERT_THROWS_CODE(PipelineExecutor(expCtx, pipeline), AssertionException, 16410);
}

TEST_F(PipelineExecutorTest, NoLogBuilder) {
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

TEST_F(PipelineExecutorTest, SerializeTest) {
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

}  // namespace
}  // namespace mongo
