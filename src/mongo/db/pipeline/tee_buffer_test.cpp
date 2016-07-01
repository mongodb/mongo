/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/tee_buffer.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

// Crutch.
bool isMongos() {
    return false;
}

namespace {

TEST(TeeBufferTest, ShouldProduceEmptyIteratorsWhenGivenNoInput) {
    auto mock = DocumentSourceMock::create();
    auto teeBuffer = TeeBuffer::create();
    teeBuffer->setSource(mock.get());
    teeBuffer->populate();

    // There are no inputs, so begin() should equal end().
    ASSERT(teeBuffer->begin() == teeBuffer->end());
}

TEST(TeeBufferTest, ShouldProvideIteratorOverSingleDocument) {
    auto inputDoc = Document{{"a", 1}};
    auto mock = DocumentSourceMock::create(inputDoc);
    auto teeBuffer = TeeBuffer::create();
    teeBuffer->setSource(mock.get());
    teeBuffer->populate();

    // Should be able to establish an iterator and get the document back.
    auto it = teeBuffer->begin();
    ASSERT(it != teeBuffer->end());
    ASSERT_DOCUMENT_EQ(*it, inputDoc);
    ++it;
    ASSERT(it == teeBuffer->end());
}

TEST(TeeBufferTest, ShouldProvideIteratorOverTwoDocuments) {
    std::deque<Document> inputDocs = {Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = DocumentSourceMock::create(inputDocs);
    auto teeBuffer = TeeBuffer::create();
    teeBuffer->setSource(mock.get());
    teeBuffer->populate();

    auto it = teeBuffer->begin();
    ASSERT(it != teeBuffer->end());
    ASSERT_DOCUMENT_EQ(*it, inputDocs.front());
    ++it;
    ASSERT(it != teeBuffer->end());
    ASSERT_DOCUMENT_EQ(*it, inputDocs.back());
    ++it;
    ASSERT(it == teeBuffer->end());
}

TEST(TeeBufferTest, ShouldBeAbleToProvideMultipleIteratorsOverTheSameInputs) {
    std::deque<Document> inputDocs = {Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = DocumentSourceMock::create(inputDocs);
    auto teeBuffer = TeeBuffer::create();
    teeBuffer->setSource(mock.get());
    teeBuffer->populate();

    auto firstIt = teeBuffer->begin();
    auto secondIt = teeBuffer->begin();

    // Advance both once.
    ASSERT(firstIt != teeBuffer->end());
    ASSERT_DOCUMENT_EQ(*firstIt, inputDocs.front());
    ++firstIt;
    ASSERT(secondIt != teeBuffer->end());
    ASSERT_DOCUMENT_EQ(*secondIt, inputDocs.front());
    ++secondIt;

    // Advance them both again.
    ASSERT(firstIt != teeBuffer->end());
    ASSERT_DOCUMENT_EQ(*firstIt, inputDocs.back());
    ++firstIt;
    ASSERT(secondIt != teeBuffer->end());
    ASSERT_DOCUMENT_EQ(*secondIt, inputDocs.back());
    ++secondIt;

    // Assert they've both reached the end.
    ASSERT(firstIt == teeBuffer->end());
    ASSERT(secondIt == teeBuffer->end());
}

TEST(TeeBufferTest, ShouldErrorWhenBufferingTooManyDocuments) {
    // Queue up at least 2000 bytes of input from a mock stage.
    std::deque<Document> inputs;
    auto largeStr = std::string(1000, 'y');
    auto inputDoc = Document{{"x", largeStr}};
    ASSERT_GTE(inputDoc.getApproximateSize(), 1000UL);
    inputs.push_back(inputDoc);
    inputs.push_back(Document{{"x", largeStr}});
    auto mock = DocumentSourceMock::create(inputs);

    const uint64_t maxMemoryUsageBytes = 1000;
    auto teeBuffer = TeeBuffer::create(maxMemoryUsageBytes);
    teeBuffer->setSource(mock.get());

    // Should exceed the configured memory limit.
    ASSERT_THROWS(teeBuffer->populate(), UserException);
}

}  // namespace
}  // namespace mongo
