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

#include "mongo/db/change_stream_pre_images_truncate_markers.h"

#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
void truncateRange(OperationContext* opCtx,
                   const CollectionPtr& preImagesColl,
                   const RecordId& minRecordId,
                   const RecordId& maxRecordId,
                   int64_t bytesDeleted,
                   int64_t docsDeleted) {
    // The session might be in use from marker initialisation so we must
    // reset it here in order to allow an untimestamped write.
    opCtx->recoveryUnit()->abandonSnapshot();
    opCtx->recoveryUnit()->allowOneUntimestampedWrite();

    WriteUnitOfWork wuow(opCtx);
    auto rs = preImagesColl->getRecordStore();
    auto status = rs->rangeTruncate(opCtx, minRecordId, maxRecordId, -bytesDeleted, -docsDeleted);
    invariantStatusOK(status);
    wuow.commit();
}

// Performs a ranged truncate over each expired marker in 'truncateMarkersForNss'. Updates the
// "Output" parameters to communicate the respective docs deleted, bytes deleted, and and maximum
// wall time of documents deleted to the caller.
void truncateExpiredMarkersForNsUUID(
    OperationContext* opCtx,
    std::shared_ptr<PreImagesTruncateMarkersPerNsUUID> truncateMarkersForNsUUID,
    const CollectionPtr& preImagesColl,
    const UUID& nsUUID,
    const RecordId& minRecordIdForNs,
    int64_t& totalDocsDeletedOutput,
    int64_t& totalBytesDeletedOutput,
    Date_t& maxWallTimeForNsTruncateOutput) {
    while (auto marker = truncateMarkersForNsUUID->peekOldestMarkerIfNeeded(opCtx)) {
        writeConflictRetry(opCtx, "truncate pre-images collection", preImagesColl->ns(), [&] {
            auto bytesDeleted = marker->bytes;
            auto docsDeleted = marker->records;

            truncateRange(opCtx,
                          preImagesColl,
                          minRecordIdForNs,
                          marker->lastRecord,
                          bytesDeleted,
                          docsDeleted);

            if (marker->wallTime > maxWallTimeForNsTruncateOutput) {
                maxWallTimeForNsTruncateOutput = marker->wallTime;
            }

            truncateMarkersForNsUUID->popOldestMarker();

            totalDocsDeletedOutput += docsDeleted;
            totalBytesDeletedOutput += bytesDeleted;
        });
    }
}

// Returns true if the pre-image with highestRecordId and highestWallTime is expired.
bool isExpired(OperationContext* opCtx,
               const boost::optional<TenantId>& tenantId,
               const RecordId& highestRecordId,
               Date_t highestWallTime) {
    auto currentTimeForTimeBasedExpiration =
        change_stream_pre_image_util::getCurrentTimeForPreImageRemoval(opCtx);

    if (tenantId) {
        // In a serverless environment, the 'expireAfterSeconds' is set per tenant and is the only
        // criteria considered when determining whether a marker is expired.
        //
        // The oldest marker is expired if:
        //   'wallTime' of the oldest marker <= current node time - 'expireAfterSeconds'
        auto expireAfterSeconds =
            Seconds{change_stream_serverless_helpers::getExpireAfterSeconds(tenantId.get())};
        auto preImageExpirationTime = currentTimeForTimeBasedExpiration - expireAfterSeconds;
        return highestWallTime <= preImageExpirationTime;
    }

    // In a non-serverless environment, a marker is expired if either:
    //     (1) 'highestWallTime' of the (partial) marker <= current node time -
    //     'expireAfterSeconds' OR
    //     (2) Timestamp of the 'highestRecordId' in the oldest marker <
    //     Timestamp of earliest oplog entry

    // The 'expireAfterSeconds' may or may not be set in a non-serverless environment.
    const auto preImageExpirationTime = change_stream_pre_image_util::getPreImageExpirationTime(
        opCtx, currentTimeForTimeBasedExpiration);
    bool expiredByTimeBasedExpiration =
        preImageExpirationTime ? highestWallTime <= preImageExpirationTime : false;

    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);
    auto highestRecordTimestamp =
        change_stream_pre_image_util::getPreImageTimestamp(highestRecordId);
    return expiredByTimeBasedExpiration || highestRecordTimestamp < currentEarliestOplogEntryTs;
}

}  // namespace

PreImagesTruncateMarkersPerNsUUID::PreImagesTruncateMarkersPerNsUUID(
    boost::optional<TenantId> tenantId,
    std::deque<Marker> markers,
    int64_t leftoverRecordsCount,
    int64_t leftoverRecordsBytes,
    int64_t minBytesPerMarker)
    : CollectionTruncateMarkersWithPartialExpiration(
          std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker),
      _tenantId(std::move(tenantId)) {}

CollectionTruncateMarkers::RecordIdAndWallTime
PreImagesTruncateMarkersPerNsUUID::getRecordIdAndWallTime(const Record& record) {
    BSONObj preImageObj = record.data.toBson();
    return CollectionTruncateMarkers::RecordIdAndWallTime(
        record.id, preImageObj[ChangeStreamPreImage::kOperationTimeFieldName].date());
}

CollectionTruncateMarkers::InitialSetOfMarkers
PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(OperationContext* opCtx,
                                                                RecordStore* rs,
                                                                const UUID& nsUUID,
                                                                RecordId& highestSeenRecordId,
                                                                Date_t& highestSeenWallTime,
                                                                int64_t minBytesPerMarker) {
    Timer scanningTimer;

    RecordIdBound minRecordIdBound =
        change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID);
    RecordId minRecordId = minRecordIdBound.recordId();

    RecordIdBound maxRecordIdBound =
        change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(nsUUID);
    RecordId maxRecordId = maxRecordIdBound.recordId();

    auto cursor = rs->getCursor(opCtx, true);
    auto record = cursor->seekNear(minRecordId);

    // A forward seekNear will return the previous entry if one does not match exactly. In most
    // cases, we will need to call next() to get our correct UUID.
    while (record && record->id < minRecordId) {
        record = cursor->next();
    }

    if (!record || (record && record->id > maxRecordId)) {
        return CollectionTruncateMarkers::InitialSetOfMarkers{
            {}, 0, 0, Microseconds{0}, MarkersCreationMethod::EmptyCollection};
    }

    int64_t currentRecords = 0;
    int64_t currentBytes = 0;
    std::deque<CollectionTruncateMarkers::Marker> markers;
    while (record && record->id < maxRecordId) {
        currentRecords++;
        currentBytes += record->data.size();

        auto [rId, wallTime] = getRecordIdAndWallTime(*record);
        highestSeenRecordId = rId;
        highestSeenWallTime = wallTime;
        if (currentBytes >= minBytesPerMarker) {
            LOGV2_DEBUG(7500500,
                        1,
                        "Marking entry as a potential future truncation point for collection with "
                        "pre-images enabled",
                        "wallTime"_attr = wallTime,
                        "nsUuid"_attr = nsUUID);

            markers.emplace_back(
                std::exchange(currentRecords, 0), std::exchange(currentBytes, 0), rId, wallTime);
        }
        record = cursor->next();
    }

    return CollectionTruncateMarkers::InitialSetOfMarkers{
        std::move(markers),
        currentRecords,
        currentBytes,
        scanningTimer.elapsed(),
        CollectionTruncateMarkers::MarkersCreationMethod::Scanning};
}

bool PreImagesTruncateMarkersPerNsUUID::_hasExcessMarkers(OperationContext* opCtx) const {
    const auto& markers = getMarkers();
    if (markers.empty()) {
        // If there's nothing in the markers queue then we don't have excess markers by definition.
        return false;
    }

    const Marker& oldestMarker = markers.front();
    return isExpired(opCtx, _tenantId, oldestMarker.lastRecord, oldestMarker.wallTime);
}

bool PreImagesTruncateMarkersPerNsUUID::_hasPartialMarkerExpired(OperationContext* opCtx) const {
    const auto& [highestSeenRecordId, highestSeenWallTime] = getPartialMarker();
    return isExpired(opCtx, _tenantId, highestSeenRecordId, highestSeenWallTime);
}

PreImagesTruncateManager::TenantTruncateMarkers
PreImagesTruncateManager::createInitialTruncateMapScanning(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionPtr& preImagesCollectionPtr) {
    auto rs = preImagesCollectionPtr->getRecordStore();

    PreImagesTruncateManager::TenantTruncateMarkers truncateMap;
    auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;
    boost::optional<UUID> currentCollectionUUID = boost::none;
    Date_t firstWallTime{};

    while ((currentCollectionUUID = change_stream_pre_image_util::findNextCollectionUUID(
                opCtx, &preImagesCollectionPtr, currentCollectionUUID, firstWallTime))) {
        Date_t highestSeenWallTime{};
        RecordId highestSeenRecordId{};
        auto initialSetOfMarkers = PreImagesTruncateMarkersPerNsUUID::createInitialMarkersScanning(
            opCtx,
            rs,
            currentCollectionUUID.get(),
            highestSeenRecordId,
            highestSeenWallTime,
            minBytesPerMarker);

        auto truncateMarkers = std::make_shared<PreImagesTruncateMarkersPerNsUUID>(
            tenantId,
            std::move(initialSetOfMarkers.markers),
            initialSetOfMarkers.leftoverRecordsCount,
            initialSetOfMarkers.leftoverRecordsBytes,
            minBytesPerMarker);

        // Enable immediate partial marker expiration by updating the highest seen recordId and
        // wallTime in case the initialisation resulted in a partially built marker.
        WriteUnitOfWork wuow(opCtx);
        truncateMarkers->updateCurrentMarkerAfterInsertOnCommit(
            opCtx, 0, highestSeenRecordId, highestSeenWallTime, 0);

        truncateMap.emplace(currentCollectionUUID.get(), truncateMarkers);
        wuow.commit();
    }

    return truncateMap;
}

void PreImagesTruncateManager::initialisePreImagesCollectionTruncateMarkers(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionPtr& preImagesCollectionPtr,
    TenantTruncateMarkersCopyOnWrite& finalTruncateMap) {

    // Initialisation requires scanning/sampling the underlying pre-images collection. To reduce the
    // amount of time writes are blocked trying to access the 'finalTruncateMap', first create a
    // temporary map whose contents will eventually replace those of the 'finalTruncateMap'.
    auto initTruncateMap = PreImagesTruncateManager::createInitialTruncateMapScanning(
        opCtx, tenantId, preImagesCollectionPtr);

    finalTruncateMap.updateWith([&](const PreImagesTruncateManager::TenantTruncateMarkers& oldMap) {
        // Critical section where no other threads can modify the 'finalTruncateMap'.

        // While scanning/sampling the pre-images collection, it's possible additional entries and
        // collections were added to the 'oldMap'.
        //
        // If the 'oldMap' contains markers for a collection UUID not captured in the
        // 'initTruncateMap', it is safe to append them to the final map.
        //
        // However, if both 'oldMap' and the 'initTruncateMap' contain truncate markers for the same
        // collection UUID, ignore the entries captured in the 'oldMap' since this initialisation is
        // best effort and is prohibited from blocking writes during startup.
        for (const auto& [nsUUID, nsTruncateMarkers] : oldMap) {
            if (initTruncateMap.find(nsUUID) == initTruncateMap.end()) {
                initTruncateMap.emplace(nsUUID, nsTruncateMarkers);
            }
        }
        return initTruncateMap;
    });
}

void PreImagesTruncateManager::ensureMarkersInitialized(OperationContext* opCtx,
                                                        boost::optional<TenantId> tenantId,
                                                        const CollectionPtr& preImagesColl) {

    auto tenantTruncateMarkers = _tenantMap.find(tenantId);
    if (!tenantTruncateMarkers) {
        tenantTruncateMarkers = _tenantMap.getOrEmplace(tenantId);
        PreImagesTruncateManager::initialisePreImagesCollectionTruncateMarkers(
            opCtx, tenantId, preImagesColl, *tenantTruncateMarkers);
    }
}

PreImagesTruncateManager::TruncateStats PreImagesTruncateManager::truncateExpiredPreImages(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionPtr& preImagesColl) {
    TruncateStats stats;
    auto tenantTruncateMarkers = _tenantMap.find(tenantId);
    if (!tenantTruncateMarkers) {
        return stats;
    }

    auto snapShottedTruncateMarkers = tenantTruncateMarkers->getUnderlyingSnapshot();
    for (auto& [nsUUID, truncateMarkersForNsUUID] : *snapShottedTruncateMarkers) {
        RecordId minRecordId =
            change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID)
                .recordId();

        int64_t docsDeletedForNs = 0;
        int64_t bytesDeletedForNs = 0;
        Date_t maxWallTimeForNsTruncate{};
        truncateExpiredMarkersForNsUUID(opCtx,
                                        truncateMarkersForNsUUID,
                                        preImagesColl,
                                        nsUUID,
                                        minRecordId,
                                        docsDeletedForNs,
                                        bytesDeletedForNs,
                                        maxWallTimeForNsTruncate);

        // Best effort for removing all expired pre-images from 'nsUUID'. If there is a partial
        // marker which can be made into an expired marker, try to remove the new marker as well.
        truncateMarkersForNsUUID->createPartialMarkerIfNecessary(opCtx);
        truncateExpiredMarkersForNsUUID(opCtx,
                                        truncateMarkersForNsUUID,
                                        preImagesColl,
                                        nsUUID,
                                        minRecordId,
                                        docsDeletedForNs,
                                        bytesDeletedForNs,
                                        maxWallTimeForNsTruncate);

        if (maxWallTimeForNsTruncate > stats.maxStartWallTime) {
            stats.maxStartWallTime = maxWallTimeForNsTruncate;
        }
        stats.docsDeleted = stats.docsDeleted + docsDeletedForNs;
        stats.bytesDeleted = stats.bytesDeleted + bytesDeletedForNs;
        stats.scannedInternalCollections++;

        // If the source collection doesn't exist and there's no more data to erase we can safely
        // remove the markers. Perform a final truncate to remove all elements just in case.
        if (CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, nsUUID) == nullptr &&
            truncateMarkersForNsUUID->isEmpty()) {

            RecordId maxRecordId =
                change_stream_pre_image_util::getAbsoluteMaxPreImageRecordIdBoundForNs(nsUUID)
                    .recordId();

            writeConflictRetry(opCtx, "final truncate", preImagesColl->ns(), [&] {
                truncateRange(opCtx, preImagesColl, minRecordId, maxRecordId, 0, 0);
            });

            tenantTruncateMarkers->erase(nsUUID);
        }
    }

    return stats;
}

void PreImagesTruncateManager::dropAllMarkersForTenant(boost::optional<TenantId> tenantId) {
    _tenantMap.erase(tenantId);
}

void PreImagesTruncateManager::updateMarkersOnInsert(OperationContext* opCtx,
                                                     boost::optional<TenantId> tenantId,
                                                     const ChangeStreamPreImage& preImage,
                                                     int64_t bytesInserted) {
    dassert(bytesInserted != 0);
    auto tenantTruncateMarkers = _tenantMap.find(tenantId);
    if (!tenantTruncateMarkers) {
        return;
    }

    auto nsUuid = preImage.getId().getNsUUID();
    auto truncateMarkersForNsUUID = tenantTruncateMarkers->find(nsUuid);

    if (!truncateMarkersForNsUUID) {
        // There are 2 possible scenarios here:
        //  (1) The 'tenantTruncateMarkers' was created, but isn't done with
        //  initialisation. In this case, the truncate markers created for 'nsUUID' may or may not
        //  be overwritten in the initialisation process. This is okay, lazy initialisation is
        //  performed by the remover thread to avoid blocking writes on startup and is strictly best
        //  effort.
        //
        //  (2) Pre-images were either recently enabled on 'nsUUID' or 'nsUUID' was just created.
        //  Either way, the first pre-images enabled insert to call 'getOrEmplace()' creates the
        //  truncate markers for the 'nsUUID'. Any following calls to 'getOrEmplace()' return a
        //  pointer to the existing truncate markers.
        truncateMarkersForNsUUID =
            tenantTruncateMarkers->getOrEmplace(nsUuid,
                                                tenantId,
                                                std::deque<CollectionTruncateMarkers::Marker>{},
                                                0,
                                                0,
                                                gPreImagesCollectionTruncateMarkersMinBytes);
    }

    auto wallTime = preImage.getOperationTime();
    auto recordId = change_stream_pre_image_util::toRecordId(preImage.getId());
    truncateMarkersForNsUUID->updateCurrentMarkerAfterInsertOnCommit(
        opCtx, bytesInserted, recordId, wallTime, 1);
}

}  // namespace mongo
