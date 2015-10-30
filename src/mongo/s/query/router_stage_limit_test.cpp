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

#include "mongo/s/query/router_stage_limit.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/query/router_stage_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(RouterStageLimitTest, LimitIsOne) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto limitStage = stdx::make_unique<RouterStageLimit>(std::move(mockStage), 1);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(!secondResult.getValue());

    // Once end-of-stream is reached, the limit stage should keep returning boost::none.
    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());
}

TEST(RouterStageLimitTest, LimitIsTwo) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));

    auto limitStage = stdx::make_unique<RouterStageLimit>(std::move(mockStage), 2);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue());
    ASSERT_EQ(*secondResult.getValue(), BSON("a" << 2));

    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());
}

TEST(RouterStageLimitTest, LimitStagePropagatesError) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto limitStage = stdx::make_unique<RouterStageLimit>(std::move(mockStage), 3);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_NOT_OK(secondResult.getStatus());
    ASSERT_EQ(secondResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(secondResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageLimitTest, LimitStageToleratesMidStreamEOF) {
    // Here we're mocking the tailable case, where there may be a boost::none returned before the
    // remote cursor is closed. Our goal is to make sure that the limit stage handles this properly,
    // not counting boost::none towards the limit.
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueEOF();
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto limitStage = stdx::make_unique<RouterStageLimit>(std::move(mockStage), 2);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(!secondResult.getValue());

    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue());
    ASSERT_EQ(*thirdResult.getValue(), BSON("a" << 2));

    auto fourthResult = limitStage->next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(!fourthResult.getValue());
}

TEST(RouterStageLimitTest, LimitStageRemotesExhausted) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->markRemotesExhausted();

    auto limitStage = stdx::make_unique<RouterStageLimit>(std::move(mockStage), 100);
    ASSERT_TRUE(limitStage->remotesExhausted());

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 1));
    ASSERT_TRUE(limitStage->remotesExhausted());

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue());
    ASSERT_EQ(*secondResult.getValue(), BSON("a" << 2));
    ASSERT_TRUE(limitStage->remotesExhausted());

    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());
    ASSERT_TRUE(limitStage->remotesExhausted());
}

TEST(RouterStageLimitTest, ForwardsAwaitDataTimeout) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    auto limitStage = stdx::make_unique<RouterStageLimit>(std::move(mockStage), 100);
    ASSERT_OK(limitStage->setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

}  // namespace

}  // namespace mongo
