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

#include "mongo/s/query/exec/router_stage_remove_metadata_fields.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/s/query/exec/router_stage_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <utility>

#include <absl/container/flat_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

// These tests use router stages, which do not actually use their OperationContext, so rather than
// going through the trouble of making one, we'll just use nullptr throughout.
OperationContext* opCtx = nullptr;

TEST(RouterStageRemoveMetadataFieldsTest, RemovesMetaDataFields) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 4 << "$sortKey" << 1 << "b" << 3));
    mockStage->queueResult(BSON("$sortKey" << BSON("" << 3) << "c" << BSON("d" << "foo")));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->queueResult(BSON("a" << 3 << "$randVal" << 4 << "$sortKey" << 2));
    mockStage->queueResult(
        BSON("$textScore" << 2 << "a" << 3 << "$randVal" << 4 << "$sortKey" << 2));
    mockStage->queueResult(BSON("$textScore" << 2));
    mockStage->queueResult(BSONObj());

    auto sortKeyStage = std::make_unique<RouterStageRemoveMetadataFields>(
        opCtx, std::move(mockStage), Document::allMetadataFieldNames);

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 4 << "b" << 3));

    auto secondResult = sortKeyStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("c" << BSON("d" << "foo")));

    auto thirdResult = sortKeyStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*thirdResult.getValue().getResult(), BSON("a" << 3));

    auto fourthResult = sortKeyStage->next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(fourthResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*fourthResult.getValue().getResult(), BSON("a" << 3));

    auto fifthResult = sortKeyStage->next();
    ASSERT_OK(fifthResult.getStatus());
    ASSERT(fifthResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*fifthResult.getValue().getResult(), BSON("a" << 3));

    auto sixthResult = sortKeyStage->next();
    ASSERT_OK(sixthResult.getStatus());
    ASSERT(sixthResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*sixthResult.getValue().getResult(), BSONObj());

    auto seventhResult = sortKeyStage->next();
    ASSERT_OK(seventhResult.getStatus());
    ASSERT(seventhResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*seventhResult.getValue().getResult(), BSONObj());

    auto eighthResult = sortKeyStage->next();
    ASSERT_OK(eighthResult.getStatus());
    ASSERT(eighthResult.getValue().isEOF());
}

TEST(RouterStageRemoveMetadataFieldsTest, PropagatesError) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("$sortKey" << 1));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));

    auto sortKeyStage = std::make_unique<RouterStageRemoveMetadataFields>(
        opCtx, std::move(mockStage), StringDataSet{"$sortKey"_sd});

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSONObj());

    auto secondResult = sortKeyStage->next();
    ASSERT_NOT_OK(secondResult.getStatus());
    ASSERT_EQ(secondResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(secondResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageRemoveMetadataFieldsTest, ToleratesMidStreamEOF) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1 << "$sortKey" << 1 << "b" << 1));
    mockStage->queueEOF();
    mockStage->queueResult(BSON("a" << 2 << "$sortKey" << 1 << "b" << 2));

    auto sortKeyStage = std::make_unique<RouterStageRemoveMetadataFields>(
        opCtx, std::move(mockStage), StringDataSet{"$sortKey"_sd});

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1 << "b" << 1));

    auto secondResult = sortKeyStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().isEOF());

    auto thirdResult = sortKeyStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*thirdResult.getValue().getResult(), BSON("a" << 2 << "b" << 2));

    auto fourthResult = sortKeyStage->next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(fourthResult.getValue().isEOF());
}

TEST(RouterStageRemoveMetadataFieldsTest, RemotesExhausted) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1 << "$sortKey" << 1 << "b" << 1));
    mockStage->queueResult(BSON("a" << 2 << "$sortKey" << 1 << "b" << 2));
    mockStage->markRemotesExhausted();

    auto sortKeyStage = std::make_unique<RouterStageRemoveMetadataFields>(
        opCtx, std::move(mockStage), StringDataSet{"$sortKey"_sd});
    ASSERT_TRUE(sortKeyStage->remotesExhausted());

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1 << "b" << 1));
    ASSERT_TRUE(sortKeyStage->remotesExhausted());

    auto secondResult = sortKeyStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 2 << "b" << 2));
    ASSERT_TRUE(sortKeyStage->remotesExhausted());

    auto thirdResult = sortKeyStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().isEOF());
    ASSERT_TRUE(sortKeyStage->remotesExhausted());
}

TEST(RouterStageRemoveMetadataFieldsTest, ForwardsAwaitDataTimeout) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    auto sortKeyStage = std::make_unique<RouterStageRemoveMetadataFields>(
        opCtx, std::move(mockStage), StringDataSet{"$sortKey"_sd});
    ASSERT_OK(sortKeyStage->setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

// Grabs the next document from a stage and ensure it matches expectedDoc.
// Assumes that remotes are exhausted after one document.
void verifyNextDocument(RouterExecStage* stage, const BSONObj& expectedDoc) {
    auto result = stage->next();
    ASSERT_OK(result.getStatus());
    ASSERT(result.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), expectedDoc);
    ASSERT_TRUE(stage->remotesExhausted());
}

TEST(RouterStageRemoveMetadataFieldsTest, AllowsNonMetaDataDollars) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("$a" << 1 << "$sortKey" << 1 << "b" << 1));
    mockStage->queueResult(BSON("a" << 2 << "$sortKey" << 1 << "$b" << 2));
    mockStage->markRemotesExhausted();

    auto sortKeyStage = std::make_unique<RouterStageRemoveMetadataFields>(
        opCtx, std::move(mockStage), StringDataSet{"$sortKey"_sd});
    ASSERT_TRUE(sortKeyStage->remotesExhausted());

    verifyNextDocument(sortKeyStage.get(), BSON("$a" << 1 << "b" << 1));
    verifyNextDocument(sortKeyStage.get(), BSON("a" << 2 << "$b" << 2));

    auto endResult = sortKeyStage->next();
    ASSERT_OK(endResult.getStatus());
    ASSERT(endResult.getValue().isEOF());
    ASSERT_TRUE(sortKeyStage->remotesExhausted());
}

// For every keyword, ensure it's removed if it's in the first, middle, or
// last position, and that the remainder of the document is undisturbed.
TEST(RouterStageRemoveMetadataFieldsTest, RemovesAllMetaDataDollars) {
    for (auto& keyword : Document::allMetadataFieldNames) {
        auto mockStage = std::make_unique<RouterStageMock>(opCtx);
        mockStage->queueResult(BSON(keyword << 1 << "$a" << 1));
        mockStage->queueResult(BSON("$a" << 1 << keyword << 1));
        mockStage->queueResult(BSON("$a" << 1 << keyword << 1 << "$b" << 1));
        mockStage->markRemotesExhausted();

        auto sortKeyStage = std::make_unique<RouterStageRemoveMetadataFields>(
            opCtx, std::move(mockStage), Document::allMetadataFieldNames);
        ASSERT_TRUE(sortKeyStage->remotesExhausted());

        verifyNextDocument(sortKeyStage.get(), BSON("$a" << 1));
        verifyNextDocument(sortKeyStage.get(), BSON("$a" << 1));
        verifyNextDocument(sortKeyStage.get(), BSON("$a" << 1 << "$b" << 1));

        auto endResult = sortKeyStage->next();
        ASSERT_OK(endResult.getStatus());
        ASSERT(endResult.getValue().isEOF());
        ASSERT_TRUE(sortKeyStage->remotesExhausted());
    }
}

}  // namespace

}  // namespace mongo
