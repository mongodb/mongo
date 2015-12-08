/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/s/query/router_stage_skip.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/query/router_stage_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(RouterStageSkipTest, SkipIsOne) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 1);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 2));

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue());
    ASSERT_EQ(*secondResult.getValue(), BSON("a" << 3));

    // Once end-of-stream is reached, the skip stage should keep returning boost::none.
    auto thirdResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());

    auto fourthResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());
}

TEST(RouterStageSkipTest, SkipIsThree) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->queueResult(BSON("a" << 4));

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 3);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 4));

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(!secondResult.getValue());
}

TEST(RouterStageSkipTest, SkipEqualToResultSetSize) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 3);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(!firstResult.getValue());
}

TEST(RouterStageSkipTest, SkipExceedsResultSetSize) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 100);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(!firstResult.getValue());
}

TEST(RouterStageSkipTest, ErrorWhileSkippingResults) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 2);

    auto firstResult = skipStage->next();
    ASSERT_NOT_OK(firstResult.getStatus());
    ASSERT_EQ(firstResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(firstResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageSkipTest, ErrorAfterSkippingResults) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 2);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 3));

    auto secondResult = skipStage->next();
    ASSERT_NOT_OK(secondResult.getStatus());
    ASSERT_EQ(secondResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(secondResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageSkipTest, SkipStageToleratesMidStreamEOF) {
    // Skip stage must propagate a boost::none, but not count it towards the skip value.
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueEOF();
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 2);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(!firstResult.getValue());

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue());
    ASSERT_EQ(*secondResult.getValue(), BSON("a" << 3));

    auto thirdResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());
}

TEST(RouterStageSkipTest, SkipStageRemotesExhausted) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->markRemotesExhausted();

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 1);
    ASSERT_TRUE(skipStage->remotesExhausted());

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 2));
    ASSERT_TRUE(skipStage->remotesExhausted());

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue());
    ASSERT_EQ(*secondResult.getValue(), BSON("a" << 3));
    ASSERT_TRUE(skipStage->remotesExhausted());

    auto thirdResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());
    ASSERT_TRUE(skipStage->remotesExhausted());
}

TEST(RouterStageSkipTest, ForwardsAwaitDataTimeout) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    auto skipStage = stdx::make_unique<RouterStageSkip>(std::move(mockStage), 3);
    ASSERT_OK(skipStage->setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

}  // namespace

}  // namespace mongo
