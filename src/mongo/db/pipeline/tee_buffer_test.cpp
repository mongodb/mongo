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
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

TEST(TeeBufferTest, ShouldRequireAtLeastOneConsumer) {
    ASSERT_THROWS_CODE(TeeBuffer::create(0), UserException, 40309);
}

TEST(TeeBufferTest, ShouldRequirePositiveBatchSize) {
    ASSERT_THROWS_CODE(TeeBuffer::create(1, 0), UserException, 40310);
    ASSERT_THROWS_CODE(TeeBuffer::create(1, -2), UserException, 40310);
}

TEST(TeeBufferTest, ShouldBeExhaustedIfInputIsExhausted) {
    auto mock = DocumentSourceMock::create();
    auto teeBuffer = TeeBuffer::create(1);
    teeBuffer->setSource(mock.get());

    ASSERT(teeBuffer->getNext(0).isEOF());
    ASSERT(teeBuffer->getNext(0).isEOF());
    ASSERT(teeBuffer->getNext(0).isEOF());
}

TEST(TeeBufferTest, ShouldProvideAllResultsWithoutPauseIfTheyFitInOneBatch) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = DocumentSourceMock::create(inputs);
    auto teeBuffer = TeeBuffer::create(1);
    teeBuffer->setSource(mock.get());

    auto next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.front().getDocument());

    next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
}

TEST(TeeBufferTest, ShouldProvideAllResultsWithoutPauseIfOnlyOneConsumer) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = DocumentSourceMock::create(inputs);

    const size_t bufferBytes = 1;  // Both docs won't fit in a single batch.
    auto teeBuffer = TeeBuffer::create(1, bufferBytes);
    teeBuffer->setSource(mock.get());

    auto next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.front().getDocument());

    next = teeBuffer->getNext(0);
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
}

TEST(TeeBufferTest, ShouldTellConsumerToPauseIfItFinishesBatchBeforeOtherConsumers) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = DocumentSourceMock::create(inputs);

    const size_t nConsumers = 2;
    const size_t bufferBytes = 1;  // Both docs won't fit in a single batch.
    auto teeBuffer = TeeBuffer::create(nConsumers, bufferBytes);
    teeBuffer->setSource(mock.get());

    auto next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.front().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());  // Consumer #1 hasn't seen the first doc yet.
    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());

    auto next1 = teeBuffer->getNext(1);
    ASSERT_TRUE(next1.isAdvanced());
    ASSERT_DOCUMENT_EQ(next1.getDocument(), inputs.front().getDocument());

    // Both consumers should be able to advance now. We'll advance consumer #1.
    next1 = teeBuffer->getNext(1);
    ASSERT_TRUE(next1.isAdvanced());
    ASSERT_DOCUMENT_EQ(next1.getDocument(), inputs.back().getDocument());

    // Consumer #1 should be blocked now. The next input is EOF, but the TeeBuffer didn't get that
    // far, since the first doc filled up the buffer.
    ASSERT_TRUE(teeBuffer->getNext(1).isPaused());
    ASSERT_TRUE(teeBuffer->getNext(1).isPaused());

    // Now exhaust consumer #0.
    next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());

    // Consumer #1 will now realize it's exhausted.
    ASSERT_TRUE(teeBuffer->getNext(1).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(1).isEOF());
}

TEST(TeeBufferTest, ShouldAllowOtherConsumersToAdvanceOnceTrailingConsumerIsDisposed) {
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"a", 1}}, Document{{"a", 2}}};
    auto mock = DocumentSourceMock::create(inputs);

    const size_t nConsumers = 2;
    const size_t bufferBytes = 1;  // Both docs won't fit in a single batch.
    auto teeBuffer = TeeBuffer::create(nConsumers, bufferBytes);
    teeBuffer->setSource(mock.get());

    auto next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.front().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());  // Consumer #1 hasn't seen the first doc yet.
    ASSERT_TRUE(teeBuffer->getNext(0).isPaused());

    // Kill consumer #1.
    teeBuffer->dispose(1);

    // Consumer #0 should be able to advance now.
    next0 = teeBuffer->getNext(0);
    ASSERT_TRUE(next0.isAdvanced());
    ASSERT_DOCUMENT_EQ(next0.getDocument(), inputs.back().getDocument());

    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
    ASSERT_TRUE(teeBuffer->getNext(0).isEOF());
}
}  // namespace
}  // namespace mongo
