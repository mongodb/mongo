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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/change_stream_options_gen.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
// Fail point to set current time for time-based expiration of pre-images.
MONGO_FAIL_POINT_DEFINE(changeStreamPreImageRemoverCurrentTime);

namespace change_stream_pre_image_util {

namespace {
// Get the 'expireAfterSeconds' from the 'ChangeStreamOptions' if not 'off', boost::none otherwise.
boost::optional<std::int64_t> getExpireAfterSecondsFromChangeStreamOptions(
    const ChangeStreamOptions& changeStreamOptions) {
    const std::variant<std::string, std::int64_t>& expireAfterSeconds =
        changeStreamOptions.getPreAndPostImages().getExpireAfterSeconds();

    if (!holds_alternative<std::string>(expireAfterSeconds)) {
        return get<std::int64_t>(expireAfterSeconds);
    }

    return boost::none;
}
}  // namespace

boost::optional<Seconds> getExpireAfterSeconds(OperationContext* opCtx) {
    const auto changeStreamOptions = ChangeStreamOptionsManager::get(opCtx).getOptions(opCtx);
    const auto expireAfterSeconds =
        getExpireAfterSecondsFromChangeStreamOptions(changeStreamOptions);
    return expireAfterSeconds ? boost::optional<Seconds>(*expireAfterSeconds) : boost::none;
}

boost::optional<Date_t> getPreImageOpTimeExpirationDate(OperationContext* opCtx,
                                                        Date_t currentTime) {
    auto expireAfterSeconds = getExpireAfterSeconds(opCtx);
    if (expireAfterSeconds) {
        return currentTime - *expireAfterSeconds;
    }

    return boost::none;
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

void truncateRange(OperationContext* opCtx,
                   const CollectionPtr& preImagesColl,
                   const RecordId& minRecordId,
                   const RecordId& maxRecordId,
                   int64_t bytesDeleted,
                   int64_t docsDeleted) {
    WriteUnitOfWork wuow(opCtx);
    auto rs = preImagesColl->getRecordStore();
    auto status = rs->rangeTruncate(opCtx,
                                    *shard_role_details::getRecoveryUnit(opCtx),
                                    minRecordId,
                                    maxRecordId,
                                    -bytesDeleted,
                                    -docsDeleted);
    invariantStatusOK(status);
    wuow.commit();
}

void truncatePreImagesByTimestampExpirationApproximation(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    Timestamp expirationTimestampApproximation) {

    const auto nsUUIDs = change_stream_pre_image_util::getNsUUIDs(opCtx, preImagesCollection);

    for (const auto& nsUUID : nsUUIDs) {
        RecordId minRecordId =
            change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID)
                .recordId();

        RecordId maxRecordIdApproximation =
            RecordIdBound(
                change_stream_pre_image_util::toRecordId(ChangeStreamPreImageId(
                    nsUUID, expirationTimestampApproximation, std::numeric_limits<int64_t>::max())))
                .recordId();

        // Truncation is based on Timestamp expiration approximation -
        // meaning there isn't a good estimate of the number of bytes and
        // documents to be truncated, so default to 0.
        truncateRange(opCtx,
                      preImagesCollection.getCollectionPtr(),
                      minRecordId,
                      maxRecordIdApproximation,
                      0,
                      0);
    }
}

UUID getPreImageNsUUID(const BSONObj& preImageObj) {
    auto parsedUUID = UUID::parse(preImageObj[ChangeStreamPreImage::kIdFieldName]
                                      .Obj()[ChangeStreamPreImageId::kNsUUIDFieldName]);
    tassert(7027400, "Pre-image collection UUID must be of UUID type", parsedUUID.isOK());
    return std::move(parsedUUID.getValue());
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

    firstDocWallTime = preImageObj[ChangeStreamPreImage::kOperationTimeFieldName].date();
    return getPreImageNsUUID(preImageObj);
}

stdx::unordered_set<UUID, UUID::Hash> getNsUUIDs(OperationContext* opCtx,
                                                 const CollectionAcquisition& preImagesCollection) {
    stdx::unordered_set<UUID, UUID::Hash> nsUUIDs;
    boost::optional<UUID> currentCollectionUUID = boost::none;
    Date_t firstWallTime{};
    while ((currentCollectionUUID = change_stream_pre_image_util::findNextCollectionUUID(
                opCtx,
                &preImagesCollection.getCollectionPtr(),
                currentCollectionUUID,
                firstWallTime))) {
        nsUUIDs.emplace(*currentCollectionUUID);
    }
    return nsUUIDs;
}

Date_t getCurrentTimeForPreImageRemoval(OperationContext* opCtx) {
    auto currentTime = opCtx->fastClockSource().now();
    changeStreamPreImageRemoverCurrentTime.execute([&](const BSONObj& data) {
        // Populate the current time for time based expiration of pre-images.
        if (auto currentTimeElem = data["currentTimeForTimeBasedExpiration"]) {
            const BSONType bsonType = currentTimeElem.type();
            if (bsonType == BSONType::string) {
                auto stringDate = currentTimeElem.String();
                currentTime = dateFromISOString(stringDate).getValue();
            } else {
                tassert(7500501,
                        str::stream()
                            << "Expected type for 'currentTimeForTimeBasedExpiration' is "
                               "'date' or a 'string' representation of ISODate, but found: "
                            << bsonType,
                        bsonType == BSONType::date);

                currentTime = currentTimeElem.Date();
            }
        }
    });

    return currentTime;
}

}  // namespace change_stream_pre_image_util
}  // namespace mongo
