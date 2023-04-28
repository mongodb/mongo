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

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

// TODO SERVER-74250: Change to slowCollectionSamplingReads once 7.0 is released.
MONGO_FAIL_POINT_DEFINE(slowOplogSamplingReads);

CollectionTruncateMarkers::CollectionTruncateMarkers(CollectionTruncateMarkers&& other) {
    stdx::lock_guard lk(other._collectionMarkersReclaimMutex);
    stdx::lock_guard lk2(other._markersMutex);

    _currentRecords.store(other._currentRecords.swap(0));
    _currentBytes.store(other._currentBytes.swap(0));
    _minBytesPerMarker = other._minBytesPerMarker;
    _markers = std::move(other._markers);
    _isDead = other._isDead;
}

bool CollectionTruncateMarkers::isDead() {
    stdx::lock_guard<Latch> lk(_collectionMarkersReclaimMutex);
    return _isDead;
}

void CollectionTruncateMarkers::kill() {
    stdx::lock_guard<Latch> lk(_collectionMarkersReclaimMutex);
    _isDead = true;
    _reclaimCv.notify_one();
}


void CollectionTruncateMarkers::awaitHasExcessMarkersOrDead(OperationContext* opCtx) {
    // Wait until kill() is called or there are too many collection markers.
    stdx::unique_lock<Latch> lock(_collectionMarkersReclaimMutex);
    while (!_isDead) {
        {
            MONGO_IDLE_THREAD_BLOCK;
            stdx::lock_guard<Latch> lk(_markersMutex);
            if (_hasExcessMarkers(opCtx)) {
                const auto& oldestMarker = _markers.front();
                invariant(oldestMarker.lastRecord.isValid());

                LOGV2_DEBUG(7393215,
                            2,
                            "Collection has excess markers",
                            "lastRecord"_attr = oldestMarker.lastRecord,
                            "wallTime"_attr = oldestMarker.wallTime);
                return;
            }
        }
        _reclaimCv.wait(lock);
    }
}

boost::optional<CollectionTruncateMarkers::Marker>
CollectionTruncateMarkers::peekOldestMarkerIfNeeded(OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lk(_markersMutex);

    if (!_hasExcessMarkers(opCtx)) {
        return {};
    }

    return _markers.front();
}

void CollectionTruncateMarkers::popOldestMarker() {
    stdx::lock_guard<Latch> lk(_markersMutex);
    _markers.pop_front();
}

CollectionTruncateMarkers::Marker& CollectionTruncateMarkers::createNewMarker(
    const RecordId& lastRecord, Date_t wallTime) {
    return _markers.emplace_back(
        _currentRecords.swap(0), _currentBytes.swap(0), lastRecord, wallTime);
}

void CollectionTruncateMarkers::createNewMarkerIfNeeded(OperationContext* opCtx,
                                                        const RecordId& lastRecord,
                                                        Date_t wallTime) {
    auto logFailedLockAcquisition = [&](const std::string& lock) {
        LOGV2_DEBUG(7393214,
                    2,
                    "Failed to acquire lock to check if a new collection marker is needed",
                    "lock"_attr = lock);
    };

    // Try to lock both mutexes, if we fail to lock a mutex then someone else is either already
    // creating a new marker or popping the oldest one. In the latter case, we let the next insert
    // trigger the new marker's creation.
    stdx::unique_lock<Latch> reclaimLk(_collectionMarkersReclaimMutex, stdx::try_to_lock);
    if (!reclaimLk) {
        logFailedLockAcquisition("_collectionMarkersReclaimMutex");
        return;
    }

    stdx::unique_lock<Latch> lk(_markersMutex, stdx::try_to_lock);
    if (!lk) {
        logFailedLockAcquisition("_markersMutex");
        return;
    }

    if (_currentBytes.load() < _minBytesPerMarker) {
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

    pokeReclaimThread(opCtx);
}

void CollectionTruncateMarkers::updateCurrentMarkerAfterInsertOnCommit(
    OperationContext* opCtx,
    int64_t bytesInserted,
    const RecordId& highestInsertedRecordId,
    Date_t wallTime,
    int64_t countInserted) {
    opCtx->recoveryUnit()->onCommit([collectionMarkers = this,
                                     bytesInserted,
                                     recordId = highestInsertedRecordId,
                                     wallTime,
                                     countInserted](OperationContext* opCtx, auto) {
        invariant(bytesInserted >= 0);
        invariant(recordId.isValid());

        collectionMarkers->_currentRecords.addAndFetch(countInserted);
        int64_t newCurrentBytes = collectionMarkers->_currentBytes.addAndFetch(bytesInserted);
        if (wallTime != Date_t() && newCurrentBytes >= collectionMarkers->_minBytesPerMarker) {
            // When other transactions commit concurrently, an uninitialized wallTime may delay
            // the creation of a new marker. This delay is limited to the number of concurrently
            // running transactions, so the size difference should be inconsequential.
            collectionMarkers->createNewMarkerIfNeeded(opCtx, recordId, wallTime);
        }
    });
}

void CollectionTruncateMarkers::clearMarkersOnCommit(OperationContext* opCtx) {
    opCtx->recoveryUnit()->onCommit([this](OperationContext*, boost::optional<Timestamp>) {
        stdx::lock_guard<Latch> lk(_markersMutex);

        _currentRecords.store(0);
        _currentBytes.store(0);
        _markers.clear();
    });
}

void CollectionTruncateMarkers::updateMarkersAfterCappedTruncateAfter(
    int64_t recordsRemoved, int64_t bytesRemoved, const RecordId& firstRemovedId) {
    stdx::lock_guard<Latch> lk(_markersMutex);

    int64_t numMarkersToRemove = 0;
    int64_t recordsInMarkersToRemove = 0;
    int64_t bytesInMarkersToRemove = 0;

    // Compute the number and associated sizes of the records from markers that are either fully or
    // partially truncated.
    for (auto it = _markers.rbegin(); it != _markers.rend(); ++it) {
        if (it->lastRecord < firstRemovedId) {
            break;
        }
        numMarkersToRemove++;
        recordsInMarkersToRemove += it->records;
        bytesInMarkersToRemove += it->bytes;
    }

    // Remove the markers corresponding to the records that were deleted.
    int64_t offset = _markers.size() - numMarkersToRemove;
    _markers.erase(_markers.begin() + offset, _markers.end());

    // Account for any remaining records from a partially truncated marker in the marker currently
    // being filled.
    _currentRecords.addAndFetch(recordsInMarkersToRemove - recordsRemoved);
    _currentBytes.addAndFetch(bytesInMarkersToRemove - bytesRemoved);
}

void CollectionTruncateMarkers::setMinBytesPerMarker(int64_t size) {
    invariant(size > 0);

    stdx::lock_guard<Latch> lk(_markersMutex);

    _minBytesPerMarker = size;
}


void CollectionTruncateMarkers::pokeReclaimThread(OperationContext* opCtx) {
    _reclaimCv.notify_one();
}

CollectionTruncateMarkers::InitialSetOfMarkers CollectionTruncateMarkers::createMarkersByScanning(
    OperationContext* opCtx,
    RecordStore* rs,
    const NamespaceString& ns,
    int64_t minBytesPerMarker,
    std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime) {
    auto startTime = curTimeMicros64();
    LOGV2_INFO(7393212,
               "Scanning collection to determine where to place markers for truncation",
               "namespace"_attr = ns);

    int64_t numRecords = 0;
    int64_t dataSize = 0;

    auto cursor = rs->getCursor(opCtx, true);

    int64_t currentRecords = 0;
    int64_t currentBytes = 0;

    std::deque<Marker> markers;

    while (auto record = cursor->next()) {
        currentRecords++;
        currentBytes += record->data.size();
        if (currentBytes >= minBytesPerMarker) {
            auto [rId, wallTime] = getRecordIdAndWallTime(*record);

            LOGV2_DEBUG(7393211,
                        1,
                        "Marking entry as a potential future truncation point",
                        "wall"_attr = wallTime);

            markers.emplace_back(
                std::exchange(currentRecords, 0), std::exchange(currentBytes, 0), rId, wallTime);
        }

        numRecords++;
        dataSize += record->data.size();
    }

    rs->updateStatsAfterRepair(opCtx, numRecords, dataSize);
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
    RecordStore* rs,
    const NamespaceString& ns,
    int64_t estimatedRecordsPerMarker,
    int64_t estimatedBytesPerMarker,
    std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime) {
    auto startTime = curTimeMicros64();

    LOGV2_INFO(7393210,
               "Sampling the collection to determine where to place markers for truncation",
               "namespace"_attr = ns);
    RecordId earliestRecordId, latestRecordId;

    {
        const bool forward = true;
        auto cursor = rs->getCursor(opCtx, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the collection just in
            // case.
            LOGV2(7393209,
                  "Failed to determine the earliest recordId, falling back to scanning the "
                  "collection",
                  "namespace"_attr = ns);
            return CollectionTruncateMarkers::createMarkersByScanning(
                opCtx, rs, ns, estimatedBytesPerMarker, std::move(getRecordIdAndWallTime));
        }
        earliestRecordId = record->id;
    }

    {
        const bool forward = false;
        auto cursor = rs->getCursor(opCtx, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the collection just in
            // case.
            LOGV2(
                7393208,
                "Failed to determine the latest recordId, falling back to scanning the collection",
                "namespace"_attr = ns);
            return CollectionTruncateMarkers::createMarkersByScanning(
                opCtx, rs, ns, estimatedBytesPerMarker, std::move(getRecordIdAndWallTime));
        }
        latestRecordId = record->id;
    }

    LOGV2(7393207,
          "Sampling from the collection to determine where to place markers for truncation",
          "namespace"_attr = ns,
          "from"_attr = earliestRecordId,
          "to"_attr = latestRecordId);

    int64_t wholeMarkers = rs->numRecords(opCtx) / estimatedRecordsPerMarker;
    // We don't use the wholeMarkers variable here due to integer division not being associative.
    // For example, 10 * (47500 / 28700) = 10, but (10 * 47500) / 28700 = 16.
    int64_t numSamples =
        (kRandomSamplesPerMarker * rs->numRecords(opCtx)) / estimatedRecordsPerMarker;

    LOGV2(7393216,
          "Taking samples and assuming each collection section contains equal amounts",
          "namespace"_attr = ns,
          "numSamples"_attr = numSamples,
          "containsNumRecords"_attr = estimatedRecordsPerMarker,
          "containsNumBytes"_attr = estimatedBytesPerMarker);

    // Divide the collection into 'wholeMarkers' logical sections, with each section containing
    // approximately 'estimatedRecordsPerMarker'. Do so by oversampling the collection, sorting the
    // samples in order of their RecordId, and then choosing the samples expected to be near the
    // right edge of each logical section.
    auto cursor = rs->getRandomCursor(opCtx);
    std::vector<RecordIdAndWallTime> collectionEstimates;
    Timer lastProgressTimer;
    for (int i = 0; i < numSamples; ++i) {
        auto samplingLogIntervalSeconds = gCollectionSamplingLogIntervalSeconds.load();
        slowOplogSamplingReads.execute(
            [&](const BSONObj& dataObj) { sleepsecs(dataObj["delay"].numberInt()); });
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the collection just in
            // case.
            LOGV2(7393206,
                  "Failed to get enough random samples, falling back to scanning the collection",
                  "namespace"_attr = ns);
            return CollectionTruncateMarkers::createMarkersByScanning(
                opCtx, rs, ns, estimatedBytesPerMarker, std::move(getRecordIdAndWallTime));
        }

        collectionEstimates.emplace_back(getRecordIdAndWallTime(*record));

        if (samplingLogIntervalSeconds > 0 &&
            lastProgressTimer.elapsed() >= Seconds(samplingLogIntervalSeconds)) {
            LOGV2(7393217,
                  "Collection sampling progress",
                  "namespace"_attr = ns,
                  "completed"_attr = (i + 1),
                  "total"_attr = numSamples);
            lastProgressTimer.reset();
        }
    }
    std::sort(
        collectionEstimates.begin(),
        collectionEstimates.end(),
        [](const RecordIdAndWallTime& a, const RecordIdAndWallTime& b) { return a.id < b.id; });
    LOGV2(7393205, "Collection sampling complete", "namespace"_attr = ns);

    std::deque<Marker> markers;
    for (int i = 1; i <= wholeMarkers; ++i) {
        // Use every (kRandomSamplesPerMarker)th sample, starting with the
        // (kRandomSamplesPerMarker - 1)th, as the last record for each marker.
        // If parsing "wall" fails, we crash to allow user to fix their collection.
        const auto& [id, wallTime] = collectionEstimates[kRandomSamplesPerMarker * i - 1];

        LOGV2_DEBUG(7393204,
                    1,
                    "Marking entry as a potential future truncation point",
                    "namespace"_attr = ns,
                    "wall"_attr = wallTime,
                    "ts"_attr = id);

        markers.emplace_back(estimatedRecordsPerMarker, estimatedBytesPerMarker, id, wallTime);
    }

    // Account for the partially filled chunk.
    auto currentRecords = rs->numRecords(opCtx) - estimatedRecordsPerMarker * wholeMarkers;
    auto currentBytes = rs->dataSize(opCtx) - estimatedBytesPerMarker * wholeMarkers;
    return CollectionTruncateMarkers::InitialSetOfMarkers{
        std::move(markers),
        currentRecords,
        currentBytes,
        Microseconds{static_cast<int64_t>(curTimeMicros64() - startTime)},
        MarkersCreationMethod::Sampling};
}

CollectionTruncateMarkers::InitialSetOfMarkers
CollectionTruncateMarkers::createFromExistingRecordStore(
    OperationContext* opCtx,
    RecordStore* rs,
    const NamespaceString& ns,
    int64_t minBytesPerMarker,
    std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime,
    boost::optional<int64_t> numberOfMarkersToKeepLegacy) {

    long long numRecords = rs->numRecords(opCtx);
    long long dataSize = rs->dataSize(opCtx);

    LOGV2(7393203,
          "The size storer reports that the collection contains",
          "numRecords"_attr = numRecords,
          "dataSize"_attr = dataSize);

    // Don't calculate markers if this is a new collection. This is to prevent standalones from
    // attempting to get a forward scanning cursor on an explicit create of the collection. These
    // values can be wrong. The assumption is that if they are both observed to be zero, there must
    // be very little data in the collection; the cost of being wrong is imperceptible.
    if (numRecords == 0 && dataSize == 0) {
        return CollectionTruncateMarkers::InitialSetOfMarkers{
            {}, 0, 0, Microseconds{0}, MarkersCreationMethod::EmptyCollection};
    }

    // Only use sampling to estimate where to place the collection markers if the number of samples
    // drawn is less than 5% of the collection.
    const uint64_t kMinSampleRatioForRandCursor = 20;

    // If the collection doesn't contain enough records to make sampling more efficient, then scan
    // the collection to determine where to put down markers.
    //
    // Unless preserving legacy behavior, compute the number of markers which would be generated
    // based on the estimated data size.
    auto numMarkers = numberOfMarkersToKeepLegacy ? numberOfMarkersToKeepLegacy.get()
                                                  : dataSize / minBytesPerMarker;
    if (numRecords <= 0 || dataSize <= 0 ||
        uint64_t(numRecords) <
            kMinSampleRatioForRandCursor * kRandomSamplesPerMarker * numMarkers) {
        return CollectionTruncateMarkers::createMarkersByScanning(
            opCtx, rs, ns, minBytesPerMarker, std::move(getRecordIdAndWallTime));
    }

    // Use the collection's average record size to estimate the number of records in each marker,
    // and thus estimate the combined size of the records.
    double avgRecordSize = double(dataSize) / double(numRecords);
    double estimatedRecordsPerMarker = std::ceil(minBytesPerMarker / avgRecordSize);
    double estimatedBytesPerMarker = estimatedRecordsPerMarker * avgRecordSize;

    return CollectionTruncateMarkers::createMarkersBySampling(opCtx,
                                                              rs,
                                                              ns,
                                                              (int64_t)estimatedRecordsPerMarker,
                                                              (int64_t)estimatedBytesPerMarker,
                                                              std::move(getRecordIdAndWallTime));
}

CollectionTruncateMarkersWithPartialExpiration::CollectionTruncateMarkersWithPartialExpiration(
    CollectionTruncateMarkersWithPartialExpiration&& other)
    : CollectionTruncateMarkers(std::move(other)) {
    stdx::lock_guard lk3(other._lastHighestRecordMutex);
    _lastHighestRecordId = std::exchange(other._lastHighestRecordId, RecordId());
    _lastHighestWallTime = std::exchange(other._lastHighestWallTime, Date_t());
}

void CollectionTruncateMarkersWithPartialExpiration::updateCurrentMarkerAfterInsertOnCommit(
    OperationContext* opCtx,
    int64_t bytesInserted,
    const RecordId& highestInsertedRecordId,
    Date_t wallTime,
    int64_t countInserted) {
    opCtx->recoveryUnit()->onCommit([collectionMarkers = this,
                                     bytesInserted,
                                     recordId = highestInsertedRecordId,
                                     wallTime,
                                     countInserted](OperationContext* opCtx, auto) {
        invariant(bytesInserted >= 0);
        invariant(recordId.isValid());

        // By putting the highest marker modification first we can guarantee than in the
        // event of a race condition between expiring a partial marker the metrics increase
        // will happen after the marker has been created. This guarantees that the metrics
        // will eventually be correct as long as the expiration criteria checks for the
        // metrics and the highest marker expiration.
        collectionMarkers->_updateHighestSeenRecordIdAndWallTime(recordId, wallTime);
        collectionMarkers->_currentRecords.addAndFetch(countInserted);
        int64_t newCurrentBytes = collectionMarkers->_currentBytes.addAndFetch(bytesInserted);
        if (newCurrentBytes >= collectionMarkers->_minBytesPerMarker) {
            collectionMarkers->createNewMarkerIfNeeded(opCtx, recordId, wallTime);
        }
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
    stdx::unique_lock<Latch> reclaimLk(_collectionMarkersReclaimMutex, stdx::try_to_lock);
    if (!reclaimLk) {
        logFailedLockAcquisition("_collectionMarkersReclaimMutex");
        return;
    }

    stdx::unique_lock<Latch> lk(_markersMutex, stdx::try_to_lock);
    if (!lk) {
        logFailedLockAcquisition("_markersMutex");
        return;
    }

    stdx::unique_lock<Latch> markerLk(_lastHighestRecordMutex, stdx::try_to_lock);
    if (!markerLk) {
        logFailedLockAcquisition("_lastHighestRecordMutex");
        return;
    }

    if (_currentBytes.load() == 0 && _currentRecords.load() == 0) {
        // Nothing can be used for a marker. Early exit now.
        return;
    }

    if (_hasPartialMarkerExpired(opCtx)) {
        auto& marker = createNewMarker(_lastHighestRecordId, _lastHighestWallTime);

        LOGV2_DEBUG(7393201,
                    2,
                    "Created a new partial collection marker",
                    "lastRecord"_attr = marker.lastRecord,
                    "wallTime"_attr = marker.wallTime,
                    "numMarkers"_attr = _markers.size());
        pokeReclaimThread(opCtx);
    }
}

void CollectionTruncateMarkersWithPartialExpiration::_updateHighestSeenRecordIdAndWallTime(
    const RecordId& rId, Date_t wallTime) {
    stdx::unique_lock lk(_lastHighestRecordMutex);
    _lastHighestRecordId = std::max(_lastHighestRecordId, rId);
    _lastHighestWallTime = std::max(_lastHighestWallTime, wallTime);
}
}  // namespace mongo
