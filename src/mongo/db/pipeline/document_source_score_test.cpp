/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/bson/json.h"
#include <boost/smart_ptr.hpp>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_score.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using DocumentSourceScoreTest = AggregationContextFixture;

TEST_F(DocumentSourceScoreTest, ErrorsIfNoScoreField) {
    auto spec = fromjson(R"({
        $score: {
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreTest, CheckNoOptionalArgsIncluded) {
    auto spec = fromjson(R"({
        $score: {
            score: "expression"
        }
    })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckAllOptionalArgsIncluded) {
    auto spec = fromjson(R"({
        $score: {
            score: "expression",
            normalizeFunction: "none",
            weight: "expression"
        }
    })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckOnlyNormalizeFunctionSpecified) {
    auto spec = fromjson(R"({
        $score: {
            score: "expression",
            normalizeFunction: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckOnlyWeightSpecified) {
    auto spec = fromjson(R"({
        $score: {
            score: "expression",
            weight: "expression"
        }
    })");

    ASSERT_DOES_NOT_THROW(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreTest, CheckIntScoreMetadataUpdated) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            weight: "expression"
        }
    })");
    Document inputDoc = Document{{"myScore", 5}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5);
}

TEST_F(DocumentSourceScoreTest, CheckDoubleScoreMetadataUpdated) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            weight: "expression"
        }
    })");
    Document inputDoc = Document{{"myScore", 5.1}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5.1);
}

TEST_F(DocumentSourceScoreTest, CheckLengthyDocScoreMetadataUpdated) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            weight: "expression"
        }
    })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"field2", 10}, {"myScore", 5.3}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 5.1
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 5.3);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfScoreNotDouble) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            weight: "expression"
        }
    })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"field2", 10}, {"myScore", "5.3"_sd}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    // Assert cannot evaluate expression into double
    ASSERT_THROWS_CODE(docSourceScore->getNext(), AssertionException, 9484101);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfExpressionFieldPathDoesNotExist) {
    auto spec = fromjson(R"({
        $score: {
            score: "$myScore",
            weight: "expression"
        }
    })");
    Document inputDoc = Document{{"field1", "hello"_sd}, {"field2", 10}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    // Assert cannot evaluate expression into double
    ASSERT_THROWS_CODE(docSourceScore->getNext(), AssertionException, 9484101);
}

TEST_F(DocumentSourceScoreTest, ErrorsIfScoreInvalidExpression) {
    auto spec = fromjson(R"({
        $score: {
            score: { $ad: ['$myScore', '$otherScore'] },
            weight: "expression"
        }
    })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", 5.3}, {"field3", true}};

    // Assert cannot parse expression
    ASSERT_THROWS_CODE(DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST_F(DocumentSourceScoreTest, ChecksScoreMetadatUpdatedValidExpression) {
    auto spec = fromjson(R"({
        $score: {
            score: { $add: ['$myScore', '$otherScore'] },
            weight: "expression"
        }
    })");
    Document inputDoc =
        Document{{"field1", "hello"_sd}, {"otherScore", 10}, {"myScore", 5.3}, {"field3", true}};

    auto docSourceScore = DocumentSourceScore::createFromBson(spec.firstElement(), getExpCtx());
    auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    docSourceScore->setSource(mock.get());

    auto next = docSourceScore->getNext();
    ASSERT(next.isAdvanced());

    // Assert inputDoc's metadata equals 15.3
    ASSERT_EQ(next.releaseDocument().metadata().getScore(), 15.3);
}
}  // namespace
}  // namespace mongo
