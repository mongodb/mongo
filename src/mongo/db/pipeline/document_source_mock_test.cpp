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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceMockTest = AggregationContextFixture;

TEST_F(DocumentSourceMockTest, OneDoc) {
    auto doc = Document{{"a", 1}};
    auto source = DocumentSourceMock::createForTest(doc, getExpCtx());
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), doc);
    ASSERT(source->getNext().isEOF());
}

TEST_F(DocumentSourceMockTest, ShouldBeConstructableFromInitializerListOfDocuments) {
    auto source =
        DocumentSourceMock::createForTest({Document{{"a", 1}}, Document{{"a", 2}}}, getExpCtx());
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), (Document{{"a", 1}}));
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), (Document{{"a", 2}}));
    ASSERT(source->getNext().isEOF());
}

TEST_F(DocumentSourceMockTest, ShouldBeConstructableFromDequeOfResults) {
    auto source =
        DocumentSourceMock::createForTest({Document{{"a", 1}},
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document{{"a", 2}}},
                                          getExpCtx());
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), (Document{{"a", 1}}));
    ASSERT_TRUE(source->getNext().isPaused());
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), (Document{{"a", 2}}));
    ASSERT(source->getNext().isEOF());
}

TEST_F(DocumentSourceMockTest, StringJSON) {
    auto source = DocumentSourceMock::createForTest("{a : 1}", getExpCtx());
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), (Document{{"a", 1}}));
    ASSERT(source->getNext().isEOF());
}

TEST_F(DocumentSourceMockTest, DequeStringJSONs) {
    auto source = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}"}, getExpCtx());
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), (Document{{"a", 1}}));
    ASSERT_DOCUMENT_EQ(source->getNext().getDocument(), (Document{{"a", 2}}));
    ASSERT(source->getNext().isEOF());
}

TEST_F(DocumentSourceMockTest, Empty) {
    auto source = DocumentSourceMock::createForTest(getExpCtx());
    ASSERT(source->getNext().isEOF());
}

TEST_F(DocumentSourceMockTest, NonTestConstructor) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(getExpCtx());

    auto source = DocumentSourceMock::create(expCtx);
    ASSERT(source->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
