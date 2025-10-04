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

#include "mongo/s/query/exec/router_stage_skip.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/query/exec/router_stage_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
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
