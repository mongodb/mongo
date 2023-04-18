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

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

class OperationContext;


// Keep "markers" against a collection to efficiently remove ranges of old records when the
// collection grows. This class is meant to be used only with collections that have the following
// requirements:
// * The collection is an insert-only collection
// * The collection has no indexes
// * If a record with RecordId=Max has to be deleted then all previous records with RecordId D such
// that Min < D <= Max should be deleted. With RecordID=Min defining a lower boundary.
//
// If these requirements hold then this class can be used to compute and maintain up-to-date markers
// for ranges of deletions. These markers will be expired and returned to the deleter whenever the
// implementation defined '_hasExcessMarkers' returns true.
class CollectionTruncateMarkers {
public:
    /** Markers represent "waypoints" of the collection that contain information between the current
     * marker and the previous one.
     *
     * Markers are created by the class automatically whenever there are more than X number of bytes
     * between the previous marker and the latest insertion.
     *
     *                                                               'partial marker'
     *            |___________________|......|____________________|______
     *               Oldest Marker               Newest Marker
     *  Min rid  <-------------------------------------------------<------- Max rid
     *
     * A 'Marker' is not created until it is full or its creation is requested by a caller. A
     * 'partial marker' is not of type 'Marker', but rather metadata counting incoming records and
     * bytes until it can be used to construct a 'Marker'.
     *
     *                    Marker
     *             |__________________|
     *                          lastRecord
     */
    struct Marker {
        int64_t records;      // Approximate number of records between the current marker and the
                              // previous marker.
        int64_t bytes;        // Approximate size of records between the current marker and the
                              // previous marker.
        RecordId lastRecord;  // RecordId of the record that created this marker.
        Date_t wallTime;      // Walltime of the record that created this marker.

        Marker(int64_t records, int64_t bytes, RecordId lastRecord, Date_t wallTime)
            : records(records),
              bytes(bytes),
              lastRecord(std::move(lastRecord)),
              wallTime(wallTime) {}
    };


    CollectionTruncateMarkers(std::deque<Marker> markers,
                              int64_t leftoverRecordsCount,
                              int64_t leftoverRecordsBytes,
                              int64_t minBytesPerMarker)
        : _minBytesPerMarker(minBytesPerMarker),
          _currentRecords(leftoverRecordsCount),
          _currentBytes(leftoverRecordsBytes),
          _markers(std::move(markers)) {}

    /**
     * Whether the instance is going to get destroyed.
     */
    bool isDead();

    /**
     * Mark this instance as serving a non-existent RecordStore. This is the case if either the
     * RecordStore has been deleted or we're shutting down. Doing this will mark the instance as
     * ready for destruction.
     */
    void kill();

    void awaitHasExcessMarkersOrDead(OperationContext* opCtx);

    boost::optional<Marker> peekOldestMarkerIfNeeded(OperationContext* opCtx) const;

    void popOldestMarker();

    void createNewMarkerIfNeeded(OperationContext* opCtx,
                                 const RecordId& lastRecord,
                                 Date_t wallTime);

    // Updates the current marker with the inserted value if the operation commits the WUOW.
    virtual void updateCurrentMarkerAfterInsertOnCommit(OperationContext* opCtx,
                                                        int64_t bytesInserted,
                                                        const RecordId& highestInsertedRecordId,
                                                        Date_t wallTime,
                                                        int64_t countInserted);

    // Clears all the markers of the instance whenever the current WUOW commits.
    void clearMarkersOnCommit(OperationContext* opCtx);

    // Updates the metadata about the collection markers after a rollback occurs.
    void updateMarkersAfterCappedTruncateAfter(int64_t recordsRemoved,
                                               int64_t bytesRemoved,
                                               const RecordId& firstRemovedId);

    // The method used for creating the initial set of markers.
    enum class MarkersCreationMethod { EmptyCollection, Scanning, Sampling };
    // The initial set of markers to use when constructing the CollectionMarkers object.
    struct InitialSetOfMarkers {
        std::deque<Marker> markers;
        int64_t leftoverRecordsCount;
        int64_t leftoverRecordsBytes;
        Microseconds timeTaken;
        MarkersCreationMethod methodUsed;
    };
    struct RecordIdAndWallTime {
        RecordId id;
        Date_t wall;

        RecordIdAndWallTime(RecordId lastRecord, Date_t wallTime)
            : id(std::move(lastRecord)), wall(std::move(wallTime)) {}
    };

    // Creates the initial set of markers. This will decide whether to perform a collection scan or
    // sampling based on the size of the collection.
    //
    // 'numberOfMarkersToKeepLegacy' exists solely to maintain legacy behavior of
    // 'OplogTruncateMarkers' previously known as 'OplogStones'. It serves as the maximum number of
    // truncate markers to keep before reclaiming the oldest truncate markers.
    static InitialSetOfMarkers createFromExistingRecordStore(
        OperationContext* opCtx,
        RecordStore* rs,
        const NamespaceString& ns,
        int64_t minBytesPerMarker,
        std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime,
        boost::optional<int64_t> numberOfMarkersToKeepLegacy = boost::none);

    // Creates the initial set of markers by fully scanning the collection. The set of markers
    // returned will have correct metrics.
    static InitialSetOfMarkers createMarkersByScanning(
        OperationContext* opCtx,
        RecordStore* rs,
        const NamespaceString& ns,
        int64_t minBytesPerMarker,
        std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime);

    // Creates the initial set of markers by sampling the collection. The set of markers
    // returned will have approximate metrics. The metrics of each marker will be equal and contain
    // the collection's size and record count divided by the number of markers.
    static InitialSetOfMarkers createMarkersBySampling(
        OperationContext* opCtx,
        RecordStore* rs,
        const NamespaceString& ns,
        int64_t estimatedRecordsPerMarker,
        int64_t estimatedBytesPerMarker,
        std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime);

    void setMinBytesPerMarker(int64_t size);

    //
    // The following methods are public only for use in tests.
    //

    size_t numMarkers() const {
        stdx::lock_guard<Latch> lk(_markersMutex);
        return _markers.size();
    }

    int64_t currentBytes() const {
        return _currentBytes.load();
    }

    int64_t currentRecords() const {
        return _currentRecords.load();
    }

private:
    friend class CollectionTruncateMarkersWithPartialExpiration;

    // Used to decide whether the oldest marker has expired. Implementations are free to use
    // whichever process they want to discern if there are expired markers.
    // This method will get called holding the _collectionMarkersReclaimMutex and _markersMutex.
    virtual bool _hasExcessMarkers(OperationContext* opCtx) const = 0;

    static constexpr uint64_t kRandomSamplesPerMarker = 10;

    Mutex _collectionMarkersReclaimMutex =
        MONGO_MAKE_LATCH("CollectionTruncateMarkers::_collectionMarkersReclaimMutex");
    stdx::condition_variable _reclaimCv;

    // True if '_rs' has been destroyed, e.g. due to repairDatabase being called on the collection's
    // database, and false otherwise.
    bool _isDead = false;

    // Minimum number of bytes the marker being filled should contain before it gets added to the
    // deque of collection markers.
    int64_t _minBytesPerMarker;

    AtomicWord<int64_t> _currentRecords;  // Number of records in the marker being filled.
    AtomicWord<int64_t> _currentBytes;    // Number of bytes in the marker being filled.

    // Protects against concurrent access to the deque of collection markers.
    mutable Mutex _markersMutex = MONGO_MAKE_LATCH("CollectionTruncateMarkers::_markersMutex");
    std::deque<Marker> _markers;  // front = oldest, back = newest.

protected:
    CollectionTruncateMarkers(CollectionTruncateMarkers&& other);

    const std::deque<Marker>& getMarkers() const {
        return _markers;
    }

    void pokeReclaimThread(OperationContext* opCtx);

    Marker& createNewMarker(const RecordId& lastRecord, Date_t wallTime);
};

/**
 * An extension of CollectionTruncateMarkers that provides support for creating "partial markers".
 *
 * Partial markers are normal markers that can be requested by the user calling
 * CollectionTruncateMarkersWithPartialExpiration::createPartialMarkerIfNecessary. The
 * implementation will then consider whether the current data awaiting a marker should be deleted
 * according to some internal logic. This is useful in time-based expiration systems as there could
 * be low activity collections containing data that should be expired but won't because there is no
 * marker.
 */
class CollectionTruncateMarkersWithPartialExpiration : public CollectionTruncateMarkers {
public:
    CollectionTruncateMarkersWithPartialExpiration(std::deque<Marker> markers,
                                                   int64_t leftoverRecordsCount,
                                                   int64_t leftoverRecordsBytes,
                                                   int64_t minBytesPerMarker)
        : CollectionTruncateMarkers(
              std::move(markers), leftoverRecordsCount, leftoverRecordsBytes, minBytesPerMarker) {}

    // Creates a partially filled marker if necessary. The criteria used is whether there is data in
    // the partial marker and whether the implementation's '_hasPartialMarkerExpired' returns true.
    void createPartialMarkerIfNecessary(OperationContext* opCtx);

    virtual void updateCurrentMarkerAfterInsertOnCommit(OperationContext* opCtx,
                                                        int64_t bytesInserted,
                                                        const RecordId& highestInsertedRecordId,
                                                        Date_t wallTime,
                                                        int64_t countInserted) final;

private:
    // Highest marker seen during the lifetime of the class. Modifications must happen
    // while holding '_lastHighestRecordMutex'.
    mutable Mutex _lastHighestRecordMutex =
        MONGO_MAKE_LATCH("CollectionTruncateMarkersWithPartialExpiration::_lastHighestRecordMutex");
    RecordId _lastHighestRecordId;
    Date_t _lastHighestWallTime;

    // Replaces the highest marker if _isMarkerLargerThanHighest returns true.
    void _replaceNewHighestMarkingIfNecessary(const RecordId& newMarkerRecordId,
                                              Date_t newMarkerWallTime);

    // Used to decide if the current partially built marker has expired.
    virtual bool _hasPartialMarkerExpired(OperationContext* opCtx) const {
        return false;
    }

protected:
    CollectionTruncateMarkersWithPartialExpiration(
        CollectionTruncateMarkersWithPartialExpiration&& other);

    std::pair<const RecordId&, const Date_t&> getPartialMarker() const {
        return {_lastHighestRecordId, _lastHighestWallTime};
    }
};

}  // namespace mongo
