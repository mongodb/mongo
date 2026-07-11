// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/router_stage_skip.h"

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

// These tests use RouterStageMock, which does not actually use its OperationContext, so rather than
// going through the trouble of making one, we'll just use nullptr throughout.
OperationContext* opCtx = nullptr;

TEST(RouterStageSkipTest, SkipIsOne) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 1);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 2));

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 3));

    // Once end-of-stream is reached, the skip stage should keep returning boost::none.
    auto thirdResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().isEOF());

    auto fourthResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().isEOF());
}

TEST(RouterStageSkipTest, SkipIsThree) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->queueResult(BSON("a" << 4));

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 3);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 4));

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().isEOF());
}

TEST(RouterStageSkipTest, SkipEqualToResultSetSize) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 3);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().isEOF());
}

TEST(RouterStageSkipTest, SkipExceedsResultSetSize) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 100);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().isEOF());
}

TEST(RouterStageSkipTest, ErrorWhileSkippingResults) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 2);

    auto firstResult = skipStage->next();
    ASSERT_NOT_OK(firstResult.getStatus());
    ASSERT_EQ(firstResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(firstResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageSkipTest, ErrorAfterSkippingResults) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 2);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 3));

    auto secondResult = skipStage->next();
    ASSERT_NOT_OK(secondResult.getStatus());
    ASSERT_EQ(secondResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(secondResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageSkipTest, SkipStageToleratesMidStreamEOF) {
    // Skip stage must propagate a boost::none, but not count it towards the skip value.
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueEOF();
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 2);

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().isEOF());

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 3));

    auto thirdResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().isEOF());
}

TEST(RouterStageSkipTest, SkipStageRemotesExhausted) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->markRemotesExhausted();

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 1);
    ASSERT_TRUE(skipStage->remotesExhausted());

    auto firstResult = skipStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 2));
    ASSERT_TRUE(skipStage->remotesExhausted());

    auto secondResult = skipStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 3));
    ASSERT_TRUE(skipStage->remotesExhausted());

    auto thirdResult = skipStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().isEOF());
    ASSERT_TRUE(skipStage->remotesExhausted());
}

TEST(RouterStageSkipTest, ForwardsAwaitDataTimeout) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    auto skipStage = std::make_unique<RouterStageSkip>(opCtx, std::move(mockStage), 3);
    ASSERT_OK(skipStage->setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

}  // namespace

}  // namespace mongo
