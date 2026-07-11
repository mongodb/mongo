// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/router_stage_limit.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/query/exec/router_stage_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

// These tests use router stages, which do not actually use their OperationContext, so rather than
// going through the trouble of making one, we'll just use nullptr throughout.
OperationContext* opCtx = nullptr;

TEST(RouterStageLimitTest, LimitIsOne) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult({BSON("a" << 1)});
    mockStage->queueResult({BSON("a" << 2)});
    mockStage->queueResult({BSON("a" << 3)});

    auto limitStage = std::make_unique<RouterStageLimit>(opCtx, std::move(mockStage), 1);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(!secondResult.getValue().getResult());

    // Once end-of-stream is reached, the limit stage should keep returning no results.
    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue().getResult());
}

TEST(RouterStageLimitTest, LimitIsTwo) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));

    auto limitStage = std::make_unique<RouterStageLimit>(opCtx, std::move(mockStage), 2);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 2));

    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue().getResult());
}

TEST(RouterStageLimitTest, LimitStagePropagatesError) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto limitStage = std::make_unique<RouterStageLimit>(opCtx, std::move(mockStage), 3);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_NOT_OK(secondResult.getStatus());
    ASSERT_EQ(secondResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(secondResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageLimitTest, LimitStageToleratesMidStreamEOF) {
    // Here we're mocking the tailable case, where there may be a boost::none returned before the
    // remote cursor is closed. Our goal is to make sure that the limit stage handles this properly,
    // not counting boost::none towards the limit.
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueEOF();
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto limitStage = std::make_unique<RouterStageLimit>(opCtx, std::move(mockStage), 2);

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().isEOF());

    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*thirdResult.getValue().getResult(), BSON("a" << 2));

    auto fourthResult = limitStage->next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(fourthResult.getValue().isEOF());
}

TEST(RouterStageLimitTest, LimitStageRemotesExhausted) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->markRemotesExhausted();

    auto limitStage = std::make_unique<RouterStageLimit>(opCtx, std::move(mockStage), 100);
    ASSERT_TRUE(limitStage->remotesExhausted());

    auto firstResult = limitStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));
    ASSERT_TRUE(limitStage->remotesExhausted());

    auto secondResult = limitStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 2));
    ASSERT_TRUE(limitStage->remotesExhausted());

    auto thirdResult = limitStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().isEOF());
    ASSERT_TRUE(limitStage->remotesExhausted());
}

TEST(RouterStageLimitTest, ForwardsAwaitDataTimeout) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    auto limitStage = std::make_unique<RouterStageLimit>(opCtx, std::move(mockStage), 100);
    ASSERT_OK(limitStage->setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

}  // namespace

}  // namespace mongo
