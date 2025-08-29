/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using DocumentSourceInternalApplyOplogUpdateTest = AggregationContextFixture;

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldBeAbleToReParseSerializedStage) {
    auto spec = fromjson(
        R"({$_internalApplyOplogUpdate: {oplogUpdate: {"$v": NumberInt(2), diff: {u: {b: 3}}}}})");

    auto stage =
        DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(), getExpCtx());

    std::vector<Value> serialization;
    stage->serializeToArray(serialization);
    auto serializedBSON = serialization[0].getDocument().toBson();
    ASSERT_VALUE_EQ(serialization[0], Value(Document(spec)));

    auto roundTripped = DocumentSourceInternalApplyOplogUpdate::createFromBson(
        serializedBSON.firstElement(), getExpCtx());

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldRejectNonObjectSpecs) {
    {
        auto spec = fromjson("{$_internalApplyOplogUpdate: 1}");
        ASSERT_THROWS_CODE(
            exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                spec.firstElement(), getExpCtx())),
            DBException,
            6315901);
    }

    {
        auto spec = fromjson("{$_internalApplyOplogUpdate: []}");

        ASSERT_THROWS_CODE(
            exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                spec.firstElement(), getExpCtx())),
            DBException,
            6315901);
    }
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldRejectMalformedSpecs) {
    auto spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
                oplogUpdate: {"$v": NumberInt(999999999), diff: {u: {b: 3}}}
           }})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       4772600);

    spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
            oplogUpdate: {"$v": "2", diff: {u: {b: 3}}}
           }})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       4772600);

    spec = fromjson(R"({$_internalApplyOplogUpdate: {oplogUpdate: {"$v": NumberInt(2)}}})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       4772601);

    spec = fromjson("{$_internalApplyOplogUpdate: {}}");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       ErrorCodes::IDLFailedToParse);

    spec =
        fromjson(R"({$_internalApplyOplogUpdate: {foo: {"$v": NumberInt(2), diff: {u: {b: 3}}}}})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       40415);

    spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
                oplogUpdate: {"$v": NumberInt(2), diff: {u: {b: 3}}},
                foo: 1
           }})");
    ASSERT_THROWS_CODE(exec::agg::buildStage(DocumentSourceInternalApplyOplogUpdate::createFromBson(
                           spec.firstElement(), getExpCtx())),
                       DBException,
                       40415);
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, UpdateMultipleDocuments) {
    auto spec = fromjson(
        R"({$_internalApplyOplogUpdate: {
                oplogUpdate: {"$v": NumberInt(2), diff: {sb: {u: {c: 3}}}}
           }})");

    auto source =
        DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStage(source);
    auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}},
                                                     Document{{"a", 1}, {"b", 1}},
                                                     Document{{"a", 2}, {"b", Document{{"c", 1}}}},
                                                     Document{{"a", 3}, {"b", Document{{"d", 2}}}}},
                                                    getExpCtx());
    stage->setSource(mock.get());

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document{{"a", 0}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    expected = Document{{"a", 1}, {"b", BSONNULL}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    expected = Document{{"a", 2}, {"b", Document{{"c", 3}}}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    expected = Document{{"a", 3}, {"b", Document{{"d", 2}, {"c", 3}}}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, ShouldErrorOnInvalidDiffs) {
    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto stage = exec::agg::buildStage(source);
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        stage->setSource(mock.get());
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770500);
    }

    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {q: {z: -7}}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto stage = exec::agg::buildStage(source);
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        stage->setSource(mock.get());
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770503);
    }

    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {"": {z: -7}}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto stage = exec::agg::buildStage(source);
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        stage->setSource(mock.get());
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770505);
    }

    {
        auto spec = fromjson(
            R"({$_internalApplyOplogUpdate: {
                    oplogUpdate: {"$v": NumberInt(2), diff: {sa: []}}
               }})");
        auto source = DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(),
                                                                             getExpCtx());
        auto stage = exec::agg::buildStage(source);
        auto mock = exec::agg::MockStage::createForTest({Document{{"a", 0}}}, getExpCtx());
        stage->setSource(mock.get());
        ASSERT_THROWS_CODE(stage->getNext(), DBException, 4770507);
    }
}

TEST_F(DocumentSourceInternalApplyOplogUpdateTest, RedactsCorrectly) {
    auto spec = fromjson(R"({
        $_internalApplyOplogUpdate: {
            oplogUpdate: {
                $v: 2,
                diff: { sa: [] }
            }
        }
    })");
    auto docSource =
        DocumentSourceInternalApplyOplogUpdate::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalApplyOplogUpdate": {
                "oplogUpdate":"?object"
            }
        })",
        redact(*docSource));
}

}  // namespace
}  // namespace mongo
