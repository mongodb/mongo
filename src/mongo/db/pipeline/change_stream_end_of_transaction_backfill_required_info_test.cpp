// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_end_of_transaction_backfill_required_info.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace {

constexpr std::string_view kBackfillFromFieldName = "backfillFrom";
constexpr std::string_view kRaisePositionFieldName = "raisePosition";

BSONObj serializeToBson(const ChangeStreamEndOfTransactionBackfillRequiredInfo& info) {
    BSONObjBuilder bob;
    info.serialize(&bob);
    return bob.obj();
}

std::shared_ptr<const ChangeStreamEndOfTransactionBackfillRequiredInfo> parseFromBson(
    const BSONObj& obj) {
    std::shared_ptr<const ErrorExtraInfo> basic =
        ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(obj);
    ASSERT(basic);
    auto info =
        std::dynamic_pointer_cast<const ChangeStreamEndOfTransactionBackfillRequiredInfo>(basic);
    ASSERT(info);
    return info;
}

const Timestamp kBackfillFrom(1234, 5);
const BSONObj kRaisePosition = BSON("_data" << "the-resume-token");

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ConstructorAndGetter) {
    ChangeStreamEndOfTransactionBackfillRequiredInfo info(kBackfillFrom, kRaisePosition);
    ASSERT_EQ(info.getBackfillFrom(), kBackfillFrom);
    ASSERT_BSONOBJ_EQ(info.getRaisePosition(), kRaisePosition);
    ASSERT_EQ(info.code, ErrorCodes::ChangeStreamEndOfTransactionBackfillRequired);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, Serialize) {
    auto obj = serializeToBson(
        ChangeStreamEndOfTransactionBackfillRequiredInfo(kBackfillFrom, kRaisePosition));
    auto backfillFrom = obj.getField(kBackfillFromFieldName);
    ASSERT_FALSE(backfillFrom.eoo());
    ASSERT_EQ(backfillFrom.type(), BSONType::timestamp);
    ASSERT_EQ(backfillFrom.timestamp(), kBackfillFrom);

    auto raisePosition = obj.getField(kRaisePositionFieldName);
    ASSERT_FALSE(raisePosition.eoo());
    ASSERT_EQ(raisePosition.type(), BSONType::object);
    ASSERT_BSONOBJ_EQ(raisePosition.Obj(), kRaisePosition);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ParseSuccess) {
    auto info = parseFromBson(
        BSON(kBackfillFromFieldName << kBackfillFrom << kRaisePositionFieldName << kRaisePosition));
    ASSERT_EQ(info->getBackfillFrom(), kBackfillFrom);
    ASSERT_BSONOBJ_EQ(info->getRaisePosition(), kRaisePosition);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ParseMissingBackfillFrom) {
    ASSERT_THROWS_CODE(ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(
                           BSON(kRaisePositionFieldName << kRaisePosition)),
                       AssertionException,
                       13877300);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ParseWrongTypeBackfillFrom) {
    ASSERT_THROWS_CODE(ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(
                           BSON(kBackfillFromFieldName << "notATimestamp" << kRaisePositionFieldName
                                                       << kRaisePosition)),
                       AssertionException,
                       13877300);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ParseDateBackfillFromRejected) {
    // Only a BSON timestamp is accepted for the backfill floor; a BSON date is rejected.
    ASSERT_THROWS_CODE(
        ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(BSON(
            kBackfillFromFieldName << Date_t::now() << kRaisePositionFieldName << kRaisePosition)),
        AssertionException,
        13877300);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ParseMissingRaisePosition) {
    ASSERT_THROWS_CODE(ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(
                           BSON(kBackfillFromFieldName << kBackfillFrom)),
                       AssertionException,
                       13877301);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ParseWrongTypeRaisePosition) {
    ASSERT_THROWS_CODE(
        ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(
            BSON(kBackfillFromFieldName << kBackfillFrom << kRaisePositionFieldName << 42)),
        AssertionException,
        13877301);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, ParseEmptyObject) {
    ASSERT_THROWS_CODE(ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(BSONObj()),
                       AssertionException,
                       13877300);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, RoundTrip) {
    ChangeStreamEndOfTransactionBackfillRequiredInfo original(kBackfillFrom, kRaisePosition);
    BSONObj serialized = serializeToBson(original);
    auto parsed = parseFromBson(serialized);
    ASSERT_EQ(parsed->getBackfillFrom(), kBackfillFrom);
    ASSERT_BSONOBJ_EQ(parsed->getRaisePosition(), kRaisePosition);
    ASSERT_BSONOBJ_EQ(serializeToBson(*parsed), serialized);
}

TEST(ChangeStreamEndOfTransactionBackfillRequiredInfo, StatusIntegration) {
    Status status(ErrorCodes::ChangeStreamEndOfTransactionBackfillRequired,
                  "Test error message",
                  serializeToBson(ChangeStreamEndOfTransactionBackfillRequiredInfo(
                      kBackfillFrom, kRaisePosition)));
    ASSERT_EQ(status.code(), ErrorCodes::ChangeStreamEndOfTransactionBackfillRequired);
    ASSERT(status.extraInfo());
    auto info = status.extraInfo<ChangeStreamEndOfTransactionBackfillRequiredInfo>();
    ASSERT(info);
    ASSERT_EQ(info->getBackfillFrom(), kBackfillFrom);
    ASSERT_BSONOBJ_EQ(info->getRaisePosition(), kRaisePosition);
}

}  // namespace
}  // namespace mongo
