/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_image_util.h"

#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/util/fail_point.h"

namespace mongo {
// Fail point to set current time for time-based expiration of pre-images.
MONGO_FAIL_POINT_DEFINE(changeStreamPreImageRemoverCurrentTime);

namespace change_stream_pre_image_util {

namespace {
// Get the 'expireAfterSeconds' from the 'ChangeStreamOptions' if not 'off', boost::none otherwise.
boost::optional<std::int64_t> getExpireAfterSecondsFromChangeStreamOptions(
    ChangeStreamOptions& changeStreamOptions) {
    const stdx::variant<std::string, std::int64_t>& expireAfterSeconds =
        changeStreamOptions.getPreAndPostImages().getExpireAfterSeconds();

    if (!stdx::holds_alternative<std::string>(expireAfterSeconds)) {
        return stdx::get<std::int64_t>(expireAfterSeconds);
    }

    return boost::none;
}
}  // namespace

boost::optional<Date_t> getPreImageExpirationTime(OperationContext* opCtx, Date_t currentTime) {
    // Non-serverless and serverless environments expire pre-images according to different logic and
    // parameters. This method retrieves the 'expireAfterSeconds' for a single-tenant environment.
    boost::optional<std::int64_t> expireAfterSeconds = boost::none;

    // Get the expiration time directly from the change stream manager.
    auto changeStreamOptions = ChangeStreamOptionsManager::get(opCtx).getOptions(opCtx);
    expireAfterSeconds = getExpireAfterSecondsFromChangeStreamOptions(changeStreamOptions);

    // A pre-image is eligible for deletion if:
    //   pre-image's op-time + expireAfterSeconds  < currentTime.
    return expireAfterSeconds ? boost::optional<Date_t>(currentTime - Seconds(*expireAfterSeconds))
                              : boost::none;
}

Timestamp getPreImageTimestamp(const RecordId& rid) {
    static constexpr auto kTopLevelFieldName = "ridAsBSON"_sd;
    auto ridAsNestedBSON = record_id_helpers::toBSONAs(rid, kTopLevelFieldName);
    // 'toBSONAs()' discards type bits of the underlying KeyString of the RecordId. However, since
    // the 'ts' field of 'ChangeStreamPreImageId' is distinct CType::kTimestamp, type bits aren't
    // necessary to obtain the original value.

    auto ridBSON = ridAsNestedBSON.getObjectField(kTopLevelFieldName);

    // Callers must ensure the 'rid' represents an underlying 'ChangeStreamPreImageId'. Otherwise,
    // the behavior of this method is undefined.
    invariant(ridBSON.hasField(ChangeStreamPreImageId::kTsFieldName));

    auto tsElem = ridBSON.getField(ChangeStreamPreImageId::kTsFieldName);
    return tsElem.timestamp();
}

RecordId toRecordId(ChangeStreamPreImageId id) {
    return record_id_helpers::keyForElem(
        BSON(ChangeStreamPreImage::kIdFieldName << id.toBSON()).firstElement());
}

RecordIdBound getAbsoluteMinPreImageRecordIdBoundForNs(const UUID& nsUUID) {
    return RecordIdBound(
        change_stream_pre_image_util::toRecordId(ChangeStreamPreImageId(nsUUID, Timestamp(), 0)));
}

RecordIdBound getAbsoluteMaxPreImageRecordIdBoundForNs(const UUID& nsUUID) {
    return RecordIdBound(change_stream_pre_image_util::toRecordId(
        ChangeStreamPreImageId(nsUUID, Timestamp::max(), std::numeric_limits<int64_t>::max())));
}

boost::optional<UUID> findNextCollectionUUID(OperationContext* opCtx,
                                             const CollectionPtr* preImagesCollPtr,
                                             boost::optional<UUID> currentNsUUID,
                                             Date_t& firstDocWallTime) {
    BSONObj preImageObj;

    // Make the minRecordId for the next collection UUID the maximum RecordId for the current
    // 'currentNsUUID'.
    auto minRecordId = currentNsUUID
        ? boost::make_optional(
              change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(
                  *currentNsUUID))
        : boost::none;
    auto planExecutor =
        InternalPlanner::collectionScan(opCtx,
                                        preImagesCollPtr,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        InternalPlanner::Direction::FORWARD,
                                        boost::none /* resumeAfterRecordId */,
                                        std::move(minRecordId));
    if (planExecutor->getNext(&preImageObj, nullptr) == PlanExecutor::IS_EOF) {
        return boost::none;
    }
    auto parsedUUID = UUID::parse(preImageObj["_id"].Obj()["nsUUID"]);
    tassert(7027400, "Pre-image collection UUID must be of UUID type", parsedUUID.isOK());

    firstDocWallTime = preImageObj[ChangeStreamPreImage::kOperationTimeFieldName].date();

    return {std::move(parsedUUID.getValue())};
}

Date_t getCurrentTimeForPreImageRemoval(OperationContext* opCtx) {
    auto currentTime = opCtx->getServiceContext()->getFastClockSource()->now();
    changeStreamPreImageRemoverCurrentTime.execute([&](const BSONObj& data) {
        // Populate the current time for time based expiration of pre-images.
        if (auto currentTimeElem = data["currentTimeForTimeBasedExpiration"]) {
            const BSONType bsonType = currentTimeElem.type();
            tassert(7500501,
                    str::stream() << "Expected type for 'currentTimeForTimeBasedExpiration' is "
                                     "'date', but found: "
                                  << bsonType,
                    bsonType == BSONType::Date);

            currentTime = currentTimeElem.Date();
        }
    });

    return currentTime;
}

}  // namespace change_stream_pre_image_util
}  // namespace mongo
