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

#include "mongo/db/pipeline/document_source_mock.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

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

// Test if we can return a control event.
TEST_F(DocumentSourceMockTest, SingleControlEvent) {
    auto orig = Document{{"_id", 0}};
    // Create a control event.
    MutableDocument doc(orig);
    doc.metadata().setChangeStreamControlEvent();

    auto source = DocumentSourceMock::createForTest(doc.freeze(), getExpCtx());
    auto next = source->getNext();
    ASSERT_FALSE(next.isEOF());
    ASSERT_FALSE(next.isAdvanced());
    ASSERT_FALSE(next.isPaused());
    ASSERT_TRUE(next.isAdvancedControlDocument());

    ASSERT_DOCUMENT_EQ(next.getDocument(), orig);
    ASSERT_TRUE(next.getDocument().metadata().isChangeStreamControlEvent());
    ASSERT_TRUE(source->getNext().isEOF());
}

// Test if we can return control events mixed with other events.
TEST_F(DocumentSourceMockTest, ControlEventsAndOtherEvents) {
    std::deque<DocumentSource::GetNextResult> docs;
    docs.push_back(Document{{"a", 1}});
    docs.push_back(DocumentSource::GetNextResult::makePauseExecution());
    docs.push_back(Document{{"a", 2}});
    {
        auto doc = Document{{"c", 1}};
        MutableDocument docBuilder(doc);
        docBuilder.metadata().setChangeStreamControlEvent();
        docs.push_back(
            DocumentSource::GetNextResult::makeAdvancedControlDocument(docBuilder.freeze()));
    }
    docs.push_back(Document{{"a", 3}});
    {
        auto doc = Document{{"c", 2}};
        MutableDocument docBuilder(doc);
        docBuilder.metadata().setChangeStreamControlEvent();
        docs.push_back(
            DocumentSource::GetNextResult::makeAdvancedControlDocument(docBuilder.freeze()));
    }

    auto source = DocumentSourceMock::createForTest(docs, getExpCtx());

    // Regular document: '{a:1}'.
    auto next = source->getNext();
    ASSERT_FALSE(next.isEOF());
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_FALSE(next.isPaused());
    ASSERT_FALSE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(next.getDocument(), (Document{{"a", 1}}));

    // Pause.
    next = source->getNext();
    ASSERT_FALSE(next.isEOF());
    ASSERT_FALSE(next.isAdvanced());
    ASSERT_TRUE(next.isPaused());
    ASSERT_FALSE(next.isAdvancedControlDocument());

    // Regular document: '{a:2}'.
    next = source->getNext();
    ASSERT_FALSE(next.isEOF());
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_FALSE(next.isPaused());
    ASSERT_FALSE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(next.getDocument(), (Document{{"a", 2}}));

    // Control document: '{c:1}'.
    next = source->getNext();
    ASSERT_FALSE(next.isEOF());
    ASSERT_FALSE(next.isAdvanced());
    ASSERT_FALSE(next.isPaused());
    ASSERT_TRUE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(next.getDocument(), (Document{{"c", 1}}));
    ASSERT_TRUE(next.getDocument().metadata().isChangeStreamControlEvent());

    // Regular document: '{a:3}'.
    next = source->getNext();
    ASSERT_FALSE(next.isEOF());
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_FALSE(next.isPaused());
    ASSERT_FALSE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(next.getDocument(), (Document{{"a", 3}}));

    // Control document: '{c:2}'.
    next = source->getNext();
    ASSERT_FALSE(next.isEOF());
    ASSERT_FALSE(next.isAdvanced());
    ASSERT_FALSE(next.isPaused());
    ASSERT_TRUE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(next.getDocument(), (Document{{"c", 2}}));
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
