// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/router_stage_transform.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/query/exec/router_stage_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// Router stages do not use their OperationContext in these tests.
OperationContext* opCtx = nullptr;

// A simple transform that adds a field "transformed: true" to every document.
BSONObj addTransformedField(BSONObj doc) {
    return doc.addField(BSON("transformed" << true).firstElement());
}

// A transform that removes the "secret" field from every document.
BSONObj removeSecretField(BSONObj doc) {
    return doc.removeField("secret");
}

TEST(RouterStageTransformTest, AppliesTransformToEachDocument) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2 << "b" << 3));
    mockStage->queueResult(BSON("x" << "hello"));

    auto stage =
        std::make_unique<RouterStageTransform>(opCtx, std::move(mockStage), addTransformedField);

    auto first = stage->next();
    ASSERT_OK(first.getStatus());
    ASSERT(first.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*first.getValue().getResult(), BSON("a" << 1 << "transformed" << true));

    auto second = stage->next();
    ASSERT_OK(second.getStatus());
    ASSERT(second.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*second.getValue().getResult(),
                      BSON("a" << 2 << "b" << 3 << "transformed" << true));

    auto third = stage->next();
    ASSERT_OK(third.getStatus());
    ASSERT(third.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*third.getValue().getResult(), BSON("x" << "hello" << "transformed" << true));

    auto eof = stage->next();
    ASSERT_OK(eof.getStatus());
    ASSERT(eof.getValue().isEOF());
}

TEST(RouterStageTransformTest, TransformCanRemoveFields) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1 << "secret" << 42));
    mockStage->queueResult(BSON("b" << 2));
    mockStage->queueResult(BSON("secret" << 99 << "c" << 3));

    auto stage =
        std::make_unique<RouterStageTransform>(opCtx, std::move(mockStage), removeSecretField);

    auto first = stage->next();
    ASSERT_OK(first.getStatus());
    ASSERT(first.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*first.getValue().getResult(), BSON("a" << 1));

    auto second = stage->next();
    ASSERT_OK(second.getStatus());
    ASSERT(second.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*second.getValue().getResult(), BSON("b" << 2));

    auto third = stage->next();
    ASSERT_OK(third.getStatus());
    ASSERT(third.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*third.getValue().getResult(), BSON("c" << 3));

    auto eof = stage->next();
    ASSERT_OK(eof.getStatus());
    ASSERT(eof.getValue().isEOF());
}

TEST(RouterStageTransformTest, PropagatesError) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::BadValue, "something went wrong"));

    auto stage =
        std::make_unique<RouterStageTransform>(opCtx, std::move(mockStage), addTransformedField);

    auto first = stage->next();
    ASSERT_OK(first.getStatus());
    ASSERT(first.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*first.getValue().getResult(), BSON("a" << 1 << "transformed" << true));

    auto second = stage->next();
    ASSERT_NOT_OK(second.getStatus());
    ASSERT_EQ(second.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(second.getStatus().reason(), "something went wrong");
}

TEST(RouterStageTransformTest, PropagatesEOF) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueEOF();
    // A result queued after EOF should not be reached.
    mockStage->queueResult(BSON("a" << 2));

    auto stage =
        std::make_unique<RouterStageTransform>(opCtx, std::move(mockStage), addTransformedField);

    auto first = stage->next();
    ASSERT_OK(first.getStatus());
    ASSERT(first.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*first.getValue().getResult(), BSON("a" << 1 << "transformed" << true));

    auto eof = stage->next();
    ASSERT_OK(eof.getStatus());
    ASSERT(eof.getValue().isEOF());
}

TEST(RouterStageTransformTest, RemotesExhausted) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(BSON("a" << 1));
    mockStage->markRemotesExhausted();

    auto stage =
        std::make_unique<RouterStageTransform>(opCtx, std::move(mockStage), addTransformedField);
    ASSERT_TRUE(stage->remotesExhausted());

    auto first = stage->next();
    ASSERT_OK(first.getStatus());
    ASSERT(first.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*first.getValue().getResult(), BSON("a" << 1 << "transformed" << true));
    ASSERT_TRUE(stage->remotesExhausted());

    auto eof = stage->next();
    ASSERT_OK(eof.getStatus());
    ASSERT(eof.getValue().isEOF());
    ASSERT_TRUE(stage->remotesExhausted());
}

TEST(RouterStageTransformTest, ForwardsAwaitDataTimeout) {
    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    auto* mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    auto stage =
        std::make_unique<RouterStageTransform>(opCtx, std::move(mockStage), addTransformedField);
    ASSERT_OK(stage->setAwaitDataTimeout(Milliseconds(500)));

    auto timeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(timeout.getStatus());
    ASSERT_EQ(500, durationCount<Milliseconds>(timeout.getValue()));
}

TEST(RouterStageTransformTest, TransformIsAppliedToNestedSubDocumentField) {
    // Simulates the recordIdsReplicated scrub use case: remove a field from a nested sub-document.
    auto removeNestedField = [](BSONObj doc) -> BSONObj {
        auto infoElem = doc["info"];
        if (infoElem.eoo() || infoElem.type() != BSONType::object) {
            return doc;
        }
        BSONObjBuilder builder;
        for (auto&& field : doc) {
            if (field.fieldNameStringData() == "info"sv) {
                builder.append("info"sv, infoElem.Obj().removeField("internal"sv));
            } else {
                builder.append(field);
            }
        }
        return builder.obj();
    };

    auto mockStage = std::make_unique<RouterStageMock>(opCtx);
    mockStage->queueResult(
        BSON("name" << "coll1" << "info" << BSON("uuid" << "abc" << "internal" << true)));
    mockStage->queueResult(BSON("name" << "coll2" << "info" << BSON("uuid" << "def")));

    auto stage =
        std::make_unique<RouterStageTransform>(opCtx, std::move(mockStage), removeNestedField);

    auto first = stage->next();
    ASSERT_OK(first.getStatus());
    ASSERT(first.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*first.getValue().getResult(),
                      BSON("name" << "coll1" << "info" << BSON("uuid" << "abc")));

    auto second = stage->next();
    ASSERT_OK(second.getStatus());
    ASSERT(second.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*second.getValue().getResult(),
                      BSON("name" << "coll2" << "info" << BSON("uuid" << "def")));

    auto eof = stage->next();
    ASSERT_OK(eof.getStatus());
    ASSERT(eof.getValue().isEOF());
}

}  // namespace
}  // namespace mongo
