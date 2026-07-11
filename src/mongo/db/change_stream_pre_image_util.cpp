// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/change_stream_pre_image_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/change_stream_options_gen.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_image_id_util.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <cstdint>
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

bool shouldUseReplicatedTruncatesForPreImages(OperationContext* opCtx) {
    // First check persistence provider.
    if (const auto& rss = rss::ReplicatedStorageService::get(opCtx);
        rss.getPersistenceProvider().shouldUseReplicatedTruncates()) {
        return true;
    }

    // Next check feature flag value.
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    return shouldUseReplicatedTruncatesForPreImages(opCtx, fcvSnapshot);
}

bool shouldUseReplicatedTruncatesForPreImages(OperationContext* opCtx,
                                              const ServerGlobalParams::FCVSnapshot& fcvSnapshot) {
    return feature_flags::gFeatureFlagUseReplicatedTruncatesForDeletions
        .isEnabledUseLastLTSFCVWhenUninitialized(VersionContext::getDecoration(opCtx), fcvSnapshot);
}

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

void truncatePreImagesByTimestampExpirationApproximation(
    OperationContext* opCtx,
    const CollectionAcquisition& preImagesCollection,
    Timestamp expirationTimestampApproximation) {
    tassert(12047105,
            "expecting no replicated truncates to be used",
            !change_stream_pre_image_util::shouldUseReplicatedTruncatesForPreImages(opCtx));

    const auto nsUUIDs = change_stream_pre_image_util::getNsUUIDs(opCtx, preImagesCollection);

    for (const auto& nsUUID : nsUUIDs) {
        RecordId minRecordId =
            change_stream_pre_image_id_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID)
                .recordId();

        RecordId maxRecordIdApproximation =
            RecordIdBound(
                change_stream_pre_image_id_util::toRecordId(ChangeStreamPreImageId(
                    nsUUID, expirationTimestampApproximation, std::numeric_limits<int64_t>::max())))
                .recordId();

        // Truncation is based on Timestamp expiration approximation -
        // meaning there isn't a good estimate of the number of bytes and
        // documents to be truncated, so default to 0.
        //
        // We deliberately skip RecordId range validation here. Validation relies on replication
        // frontiers (allDurable / lastApplied) being initialized, which is not the case yet during
        // this early recovery window. Using those frontiers would incorrectly reject this truncate.
        // This approximation-based, unreplicated truncate is a transitional mechanism and is
        // expected to be removed once pre-images are using replicated truncates.
        repl::UnreplicatedWritesBlock uwb(opCtx);
        WriteUnitOfWork wuow(opCtx);
        collection_internal::truncateRange(opCtx,
                                           preImagesCollection.getCollectionPtr(),
                                           minRecordId,
                                           maxRecordIdApproximation,
                                           0,
                                           0,
                                           false /* shouldValidateRecordIdRange */);
        wuow.commit();
    }
}

boost::optional<UUID> findNextCollectionUUID(OperationContext* opCtx,
                                             const CollectionAcquisition& preImagesColl,
                                             boost::optional<UUID> currentNsUUID,
                                             Date_t& firstDocWallTime) {
    BSONObj preImageObj;

    // Make the minRecordId for the next collection UUID the maximum RecordId for the current
    // 'currentNsUUID'.
    auto minRecordId = currentNsUUID
        ? boost::make_optional(
              change_stream_pre_image_id_util::getAbsoluteMaxPreImageRecordIdBoundForNs(
                  *currentNsUUID))
        : boost::none;
    auto planExecutor =
        InternalPlanner::collectionScan(opCtx,
                                        preImagesColl,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        InternalPlanner::Direction::FORWARD,
                                        boost::none /* resumeAfterRecordId */,
                                        std::move(minRecordId));
    if (planExecutor->getNext(&preImageObj, nullptr) == PlanExecutor::IS_EOF) {
        return boost::none;
    }

    firstDocWallTime = preImageObj[ChangeStreamPreImage::kOperationTimeFieldName].date();
    return change_stream_pre_image_id_util::getPreImageNsUUID(preImageObj);
}

stdx::unordered_set<UUID, UUID::Hash> getNsUUIDs(OperationContext* opCtx,
                                                 const CollectionAcquisition& preImagesCollection) {
    stdx::unordered_set<UUID, UUID::Hash> nsUUIDs;
    boost::optional<UUID> currentCollectionUUID = boost::none;
    Date_t firstWallTime{};
    while ((currentCollectionUUID = change_stream_pre_image_util::findNextCollectionUUID(
                opCtx, preImagesCollection, currentCollectionUUID, firstWallTime))) {
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
