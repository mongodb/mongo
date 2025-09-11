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

#include "mongo/db/storage/collection_truncate_markers.h"

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/timer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

// Strings for MarkerCreationMethods.
static constexpr StringData kEmptyCollectionString = "emptyCollection"_sd;
static constexpr StringData kScanningString = "scanning"_sd;
static constexpr StringData kSamplingString = "sampling"_sd;
static constexpr StringData kInProgressString = "inProgress"_sd;
}  // namespace

StringData CollectionTruncateMarkers::toString(
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod) {
    switch (creationMethod) {
        case CollectionTruncateMarkers::MarkersCreationMethod::EmptyCollection:
            return kEmptyCollectionString;
        case CollectionTruncateMarkers::MarkersCreationMethod::Scanning:
            return kScanningString;
        case CollectionTruncateMarkers::MarkersCreationMethod::Sampling:
            return kSamplingString;
        case CollectionTruncateMarkers::MarkersCreationMethod::InProgress:
            return kInProgressString;
        default:
            MONGO_UNREACHABLE;
    }
}

boost::optional<CollectionTruncateMarkers::Marker>
CollectionTruncateMarkers::peekOldestMarkerIfNeeded(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lk(_markersMutex);

    if (!_hasExcessMarkers(opCtx)) {
        return {};
    }

    return _markers.front();
}

void CollectionTruncateMarkers::popOldestMarker() {
    stdx::lock_guard<stdx::mutex> lk(_markersMutex);
    _markers.pop_front();
}

CollectionTruncateMarkers::Marker& CollectionTruncateMarkers::createNewMarker(
    const RecordId& lastRecord, Date_t wallTime) {
    return _markers.emplace_back(
        _currentRecords.swap(0), _currentBytes.swap(0), lastRecord, wallTime);
}

void CollectionTruncateMarkers::createNewMarkerIfNeeded(const RecordId& lastRecord,
                                                        Date_t wallTime) {
    auto logFailedLockAcquisition = [&](const std::string& lock) {
        LOGV2_DEBUG(7393214,
                    2,
                    "Failed to acquire lock to check if a new collection marker is needed",
                    "lock"_attr = lock);
    };

    // Try to lock the mutex, if we fail to lock then someone else is either already creating a new
    // marker or popping the oldest one. In the latter case, we let the next insert trigger the new
    // marker's creation.
    stdx::unique_lock<stdx::mutex> lk(_markersMutex, stdx::try_to_lock);
    if (!lk) {
        logFailedLockAcquisition("_markersMutex");
        return;
    }

    if (feature_flags::gOplogSamplingAsyncEnabled.isEnabled() && !_initialSamplingFinished) {
        // Must have finished creating initial markers first.
        return;
    }

    if (_currentBytes.load() < _minBytesPerMarker.load()) {
        // Must have raced to create a new marker, someone else already triggered it.
        return;
    }

    if (!_markers.empty() && lastRecord < _markers.back().lastRecord) {
        // Skip creating a new marker when the record's position comes before the most recently
        // created marker. We likely raced with another batch of inserts that caused us to try and
        // make multiples markers.
        return;
    }

    auto& marker = createNewMarker(lastRecord, wallTime);

    LOGV2_DEBUG(7393213,
                2,
                "Created a new collection marker",
                "lastRecord"_attr = marker.lastRecord,
                "wallTime"_attr = marker.wallTime,
                "numMarkers"_attr = _markers.size());

    _notifyNewMarkerCreation();
}

void CollectionTruncateMarkers::updateCurrentMarkerAfterInsertOnCommit(
    OperationContext* opCtx,
    int64_t bytesInserted,
    const RecordId& highestInsertedRecordId,
    Date_t wallTime,
    int64_t countInserted) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [collectionMarkers = shared_from_this(),
         bytesInserted,
         recordId = highestInsertedRecordId,
         wallTime,
         countInserted](OperationContext* opCtx, auto) {
            invariant(bytesInserted >= 0);
            invariant(recordId.isValid());

            collectionMarkers->_currentRecords.addAndFetch(countInserted);
            int64_t newCurrentBytes = collectionMarkers->_currentBytes.addAndFetch(bytesInserted);
            if (wallTime != Date_t() &&
                newCurrentBytes >= collectionMarkers->_minBytesPerMarker.load()) {
                // When other transactions commit concurrently, an uninitialized wallTime may delay
                // the creation of a new marker. This delay is limited to the number of concurrently
                // running transactions, so the size difference should be inconsequential.
                collectionMarkers->createNewMarkerIfNeeded(recordId, wallTime);
            }
        });
}

void CollectionTruncateMarkers::setMinBytesPerMarker(int64_t size) {
    invariant(size > 0);
    _minBytesPerMarker.store(size);
}

void CollectionTruncateMarkers::initialSamplingFinished() {
    stdx::lock_guard<stdx::mutex> lk(_markersMutex);
    _initialSamplingFinished = true;
}

CollectionTruncateMarkers::InitialSetOfMarkers CollectionTruncateMarkers::createMarkersByScanning(
    OperationContext* opCtx,
    CollectionIterator& collectionIterator,
    int64_t minBytesPerMarker,
    std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime) {
    auto startTime = curTimeMicros64();
    LOGV2_INFO(7393212,
               "Scanning collection to determine where to place markers for truncation",
               "uuid"_attr = collectionIterator.getRecordStore()->uuid());

    int64_t numRecords = 0;
    int64_t dataSize = 0;
    int64_t currentRecords = 0;
    int64_t currentBytes = 0;

    std::deque<Marker> markers;

    while (auto nextRecord = collectionIterator.getNext()) {
        const auto& [rId, doc] = *nextRecord;
        currentRecords++;
        currentBytes += doc.objsize();
        if (currentBytes >= minBytesPerMarker) {
            auto [_, wallTime] =
                getRecordIdAndWallTime(Record{rId, RecordData{doc.objdata(), doc.objsize()}});

            LOGV2_DEBUG(7393211,
                        1,
                        "Marking entry as a potential future truncation point",
                        "wall"_attr = wallTime);

            markers.emplace_back(
                std::exchange(currentRecords, 0), std::exchange(currentBytes, 0), rId, wallTime);
        }

        numRecords++;
        dataSize += doc.objsize();
    }

    collectionIterator.getRecordStore()->updateStatsAfterRepair(numRecords, dataSize);
    auto endTime = curTimeMicros64();
    return CollectionTruncateMarkers::InitialSetOfMarkers{
        std::move(markers),
        currentRecords,
        currentBytes,
        Microseconds{static_cast<int64_t>(endTime - startTime)},
        MarkersCreationMethod::Scanning};
}


CollectionTruncateMarkers::InitialSetOfMarkers CollectionTruncateMarkers::createMarkersBySampling(
    OperationContext* opCtx,
    CollectionIterator& collectionIterator,
    int64_t estimatedRecordsPerMarker,
    int64_t estimatedBytesPerMarker,
    std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime,
    TickSource* tickSource) {
    auto startTime = curTimeMicros64();

    LOGV2_INFO(7393210,
               "Sampling the collection to determine where to place markers for truncation",
               "uuid"_attr = collectionIterator.getRecordStore()->uuid());
    RecordId earliestRecordId, latestRecordId;

    {
        auto record = [&] {
            const bool forward = true;
            auto rs = collectionIterator.getRecordStore();
            return rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward)
                ->next();
        }();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the collection just in
            // case.
            LOGV2(7393209,
                  "Failed to determine the earliest recordId, falling back to scanning the "
                  "collection",
                  "uuid"_attr = collectionIterator.getRecordStore()->uuid());
            return CollectionTruncateMarkers::createMarkersByScanning(
                opCtx,
                collectionIterator,
                estimatedBytesPerMarker,
                std::move(getRecordIdAndWallTime));
        }
        earliestRecordId = record->id;
    }

    {
        auto record = [&] {
            const bool forward = false;
            auto rs = collectionIterator.getRecordStore();
            return rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), forward)
                ->next();
        }();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the collection just in
            // case.
            LOGV2(
                7393208,
                "Failed to determine the latest recordId, falling back to scanning the collection",
                "uuid"_attr = collectionIterator.getRecordStore()->uuid());
            return CollectionTruncateMarkers::createMarkersByScanning(
                opCtx,
                collectionIterator,
                estimatedBytesPerMarker,
                std::move(getRecordIdAndWallTime));
        }
        latestRecordId = record->id;
    }

    LOGV2(7393207,
          "Sampling from the collection to determine where to place markers for truncation",
          "uuid"_attr = collectionIterator.getRecordStore()->uuid(),
          "from"_attr = earliestRecordId,
          "to"_attr = latestRecordId);

    int64_t wholeMarkers = collectionIterator.numRecords() / estimatedRecordsPerMarker;
    // We don't use the wholeMarkers variable here due to integer division not being associative.
    // For example, 10 * (47500 / 28700) = 10, but (10 * 47500) / 28700 = 16.
    int64_t numSamples =
        (CollectionTruncateMarkers::kRandomSamplesPerMarker * collectionIterator.numRecords()) /
        estimatedRecordsPerMarker;

    LOGV2(7393216,
          "Taking samples and assuming each collection section contains equal amounts",
          "uuid"_attr = collectionIterator.getRecordStore()->uuid(),
          "numSamples"_attr = numSamples,
          "containsNumRecords"_attr = estimatedRecordsPerMarker,
          "containsNumBytes"_attr = estimatedBytesPerMarker);

    // Divide the collection into 'wholeMarkers' logical sections, with each section containing
    // approximately 'estimatedRecordsPerMarker'. Do so by oversampling the collection, sorting the
    // samples in order of their RecordId, and then choosing the samples expected to be near the
    // right edge of each logical section.

    std::vector<RecordIdAndWallTime> collectionEstimates;
    Timer lastProgressTimer(tickSource);

    for (int i = 0; i < numSamples; ++i) {
        auto nextRandom = collectionIterator.getNextRandom();
        const auto [rId, doc] = *nextRandom;
        auto samplingLogIntervalSeconds = gCollectionSamplingLogIntervalSeconds.load();
        if (!nextRandom) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the collection just in
            // case.
            LOGV2(7393206,
                  "Failed to get enough random samples, falling back to scanning the collection",
                  "uuid"_attr = collectionIterator.getRecordStore()->uuid());
            collectionIterator.reset(opCtx);
            return CollectionTruncateMarkers::createMarkersByScanning(
                opCtx,
                collectionIterator,
                estimatedBytesPerMarker,
                std::move(getRecordIdAndWallTime));
        }

        collectionEstimates.emplace_back(
            getRecordIdAndWallTime(Record{rId, RecordData{doc.objdata(), doc.objsize()}}));

        if (samplingLogIntervalSeconds > 0 &&
            lastProgressTimer.elapsed() >= Seconds(samplingLogIntervalSeconds)) {
            LOGV2(7393217,
                  "Collection sampling progress",
                  "uuid"_attr = collectionIterator.getRecordStore()->uuid(),
                  "completed"_attr = (i + 1),
                  "total"_attr = numSamples);
            lastProgressTimer.reset();
        }
    }

    std::sort(collectionEstimates.begin(),
              collectionEstimates.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    LOGV2(7393205,
          "Collection sampling complete",
          "uuid"_attr = collectionIterator.getRecordStore()->uuid());

    std::deque<Marker> markers;
    for (int i = 1; i <= wholeMarkers; ++i) {
        // Use every (kRandomSamplesPerMarker)th sample, starting with the
        // (kRandomSamplesPerMarker - 1)th, as the last record for each marker.
        // If parsing "wall" fails, we crash to allow user to fix their collection.
        const auto& [id, wallTime] = collectionEstimates[kRandomSamplesPerMarker * i - 1];

        LOGV2_DEBUG(7393204,
                    1,
                    "Marking entry as a potential future truncation point",
                    "uuid"_attr = collectionIterator.getRecordStore()->uuid(),
                    "wall"_attr = wallTime,
                    "ts"_attr = id);

        markers.emplace_back(estimatedRecordsPerMarker, estimatedBytesPerMarker, id, wallTime);
    }

    // Account for the partially filled chunk.
    auto currentRecords =
        collectionIterator.numRecords() - estimatedRecordsPerMarker * wholeMarkers;
    auto currentBytes = collectionIterator.dataSize() - estimatedBytesPerMarker * wholeMarkers;
    auto duration = static_cast<int64_t>(curTimeMicros64() - startTime);
    LOGV2_DEBUG(10621100, 1, "createMarkersBySampling finished", "durationMicros"_attr = duration);
    return CollectionTruncateMarkers::InitialSetOfMarkers{std::move(markers),
                                                          currentRecords,
                                                          currentBytes,
                                                          Microseconds{duration},
                                                          MarkersCreationMethod::Sampling};
}

CollectionTruncateMarkers::MarkersCreationMethod
CollectionTruncateMarkers::computeInitialCreationMethod(
    int64_t numRecords,
    int64_t dataSize,
    int64_t minBytesPerMarker,
    boost::optional<int64_t> numberOfMarkersToKeepForOplog) {
    // Don't calculate markers if this is a new collection. This is to prevent standalones from
    // attempting to get a forward scanning cursor on an explicit create of the collection. These
    // values can be wrong. The assumption is that if they are both observed to be zero, there must
    // be very little data in the collection; the cost of being wrong is imperceptible.
    if (numRecords == 0 && dataSize == 0) {
        return MarkersCreationMethod::EmptyCollection;
    }

    // Only use sampling to estimate where to place the collection markers if the number of samples
    // drawn is less than 5% of the collection.
    const uint64_t kMinSampleRatioForRandCursor = 20;

    // If the collection doesn't contain enough records to make sampling more efficient, then scan
    // the collection to determine where to put down markers.
    //
    // Unless preserving legacy behavior of 'OplogTruncateMarkers', compute the number of markers
    // which would be generated based on the estimated data size.
    auto numMarkers = numberOfMarkersToKeepForOplog ? numberOfMarkersToKeepForOplog.get()
                                                    : dataSize / minBytesPerMarker;
    if (numRecords <= 0 || dataSize <= 0 ||
        uint64_t(numRecords) <
            kMinSampleRatioForRandCursor * kRandomSamplesPerMarker * numMarkers) {
        return MarkersCreationMethod::Scanning;
    }

    return MarkersCreationMethod::Sampling;
}

CollectionTruncateMarkers::InitialSetOfMarkers
CollectionTruncateMarkers::createFromCollectionIterator(
    OperationContext* opCtx,
    CollectionIterator& collectionIterator,
    int64_t minBytesPerMarker,
    std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime,
    boost::optional<int64_t> numberOfMarkersToKeepForOplog) {

    long long numRecords = collectionIterator.numRecords();
    long long dataSize = collectionIterator.dataSize();

    LOGV2(7393203,
          "The size storer reports that the collection contains",
          "numRecords"_attr = numRecords,
          "dataSize"_attr = dataSize);

    auto creationMethod = CollectionTruncateMarkers::computeInitialCreationMethod(
        numRecords, dataSize, minBytesPerMarker, numberOfMarkersToKeepForOplog);

    switch (creationMethod) {
        case MarkersCreationMethod::EmptyCollection:
            // Don't calculate markers if this is a new collection. This is to prevent standalones
            // from attempting to get a forward scanning cursor on an explicit create of the
            // collection. These values can be wrong. The assumption is that if they are both
            // observed to be zero, there must be very little data in the collection; the cost of
            // being wrong is imperceptible.
            return CollectionTruncateMarkers::InitialSetOfMarkers{
                {}, 0, 0, Microseconds{0}, MarkersCreationMethod::EmptyCollection};
        case MarkersCreationMethod::Scanning:
            return CollectionTruncateMarkers::createMarkersByScanning(
                opCtx, collectionIterator, minBytesPerMarker, std::move(getRecordIdAndWallTime));
        default: {
            // Use the collection's average record size to estimate the number of records in each
            // marker,
            // and thus estimate the combined size of the records.
            double avgRecordSize = double(dataSize) / double(numRecords);
            double estimatedRecordsPerMarker = std::ceil(minBytesPerMarker / avgRecordSize);
            double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;

            return CollectionTruncateMarkers::createMarkersBySampling(
                opCtx,
                collectionIterator,
                (int64_t)estimatedRecordsPerMarker,
                (int64_t)estimatedBytesPerMarker,
                std::move(getRecordIdAndWallTime));
        }
    }
}

void CollectionTruncateMarkersWithPartialExpiration::updateCurrentMarkerAfterInsertOnCommit(
    OperationContext* opCtx,
    int64_t bytesInserted,
    const RecordId& highestInsertedRecordId,
    Date_t wallTime,
    int64_t countInserted) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [collectionMarkers =
             std::static_pointer_cast<CollectionTruncateMarkersWithPartialExpiration>(
                 shared_from_this()),
         bytesInserted,
         recordId = highestInsertedRecordId,
         wallTime,
         countInserted](OperationContext* opCtx, auto) {
            invariant(bytesInserted >= 0);
            invariant(recordId.isValid());
            collectionMarkers->updateCurrentMarker(
                bytesInserted, recordId, wallTime, countInserted);
        });
}

void CollectionTruncateMarkersWithPartialExpiration::createPartialMarkerIfNecessary(
    OperationContext* opCtx) {
    auto logFailedLockAcquisition = [&](const std::string& lock) {
        LOGV2_DEBUG(7393202,
                    2,
                    "Failed to acquire lock to check if a new partial collection marker is needed",
                    "lock"_attr = lock);
    };

    // Try to lock all mutexes, if we fail to lock a mutex then someone else is either already
    // creating a new marker or popping the oldest one. In the latter case, we let the next check
    // trigger the new partial marker's creation.

    stdx::unique_lock<stdx::mutex> lk(_markersMutex, stdx::try_to_lock);
    if (!lk) {
        logFailedLockAcquisition("_markersMutex");
        return;
    }

    stdx::unique_lock<stdx::mutex> highestRecordLock(_highestRecordMutex, stdx::try_to_lock);
    if (!highestRecordLock) {
        logFailedLockAcquisition("_highestRecordMutex");
        return;
    }

    if (_currentBytes.load() == 0 && _currentRecords.load() == 0) {
        // Nothing can be used for a marker. Early exit now.
        return;
    }

    if (_hasPartialMarkerExpired(opCtx, _highestRecordId, _highestWallTime)) {
        auto& marker = createNewMarker(_highestRecordId, _highestWallTime);

        LOGV2_DEBUG(7393201,
                    2,
                    "Created a new partial collection marker",
                    "lastRecord"_attr = marker.lastRecord,
                    "wallTime"_attr = marker.wallTime,
                    "numMarkers"_attr = _markers.size());
        _notifyNewMarkerCreation();
    }
}

void CollectionTruncateMarkersWithPartialExpiration::_updateHighestSeenRecordIdAndWallTime(
    const RecordId& rId, Date_t wallTime) {
    stdx::unique_lock lk(_highestRecordMutex);
    if (_highestRecordId < rId) {
        _highestRecordId = rId;
    }
    if (_highestWallTime < wallTime) {
        _highestWallTime = wallTime;
    }
}

void CollectionTruncateMarkersWithPartialExpiration::updateCurrentMarker(
    int64_t bytesAdded,
    const RecordId& highestRecordId,
    Date_t highestWallTime,
    int64_t numRecordsAdded) {
    // By putting the highest marker modification first we can guarantee than in the
    // event of a race condition between expiring a partial marker the metrics increase
    // will happen after the marker has been created. This guarantees that the metrics
    // will eventually be correct as long as the expiration criteria checks for the
    // metrics and the highest marker expiration.
    _updateHighestSeenRecordIdAndWallTime(highestRecordId, highestWallTime);
    _currentRecords.addAndFetch(numRecordsAdded);
    int64_t newCurrentBytes = _currentBytes.addAndFetch(bytesAdded);
    if (highestWallTime != Date_t() && highestRecordId.isValid() &&
        newCurrentBytes >= _minBytesPerMarker.load()) {
        createNewMarkerIfNeeded(highestRecordId, highestWallTime);
    }
}

}  // namespace mongo
