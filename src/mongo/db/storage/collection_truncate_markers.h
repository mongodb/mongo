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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
class CollectionTruncateMarkers : public std::enable_shared_from_this<CollectionTruncateMarkers> {
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

    // The method used for creating the initial set of markers.
    enum class MarkersCreationMethod { EmptyCollection, Scanning, Sampling, InProgress };

    CollectionTruncateMarkers(std::deque<Marker> markers,
                              int64_t leftoverRecordsCount,
                              int64_t leftoverRecordsBytes,
                              int64_t minBytesPerMarker,
                              Microseconds totalTimeSpentBuilding,
                              MarkersCreationMethod creationMethod)
        : _minBytesPerMarker(minBytesPerMarker),
          _currentRecords(leftoverRecordsCount),
          _currentBytes(leftoverRecordsBytes),
          _markers(std::move(markers)),
          _totalTimeProcessing(totalTimeSpentBuilding),
          _creationMethod(creationMethod) {}

    virtual ~CollectionTruncateMarkers() = default;

    boost::optional<Marker> peekOldestMarkerIfNeeded(OperationContext* opCtx) const;

    void popOldestMarker();

    void createNewMarkerIfNeeded(const RecordId& lastRecord, Date_t wallTime);

    // Updates the current marker with the inserted value if the operation commits the WUOW.
    virtual void updateCurrentMarkerAfterInsertOnCommit(OperationContext* opCtx,
                                                        int64_t bytesInserted,
                                                        const RecordId& highestInsertedRecordId,
                                                        Date_t wallTime,
                                                        int64_t countInserted);

    /**
     * Waits for expired markers. See _hasExcessMarkers().
     * Returns true if expired markers are present.
     * Otherwise, returns false. This could be due to reaching an implementation defined
     * deadline for the wait, or if we are shutting down this CollectionTruncateMarkers
     * instance.
     * This operation may throw an exception if interrupted.
     * Storage engines supporting oplog truncate markers must implement this function.
     */
    virtual bool awaitHasExcessMarkersOrDead(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    static StringData toString(MarkersCreationMethod creationMethod);

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

    // Given the estimated collection 'dataSize' and 'numRecords', along with a target
    // 'minBytesPerMarker' and the desired 'numRandomSamplesPerMarker' (if sampling is the chosen
    // creation method), computes the initial creation method to try for the initialization.
    //
    // It's possible the initial creation method is not the actual creation method. However, it will
    // be the first creation method tried. For example, if estimates of 'dataSize' and 'numRecords'
    // are really far off, sampling may default back to scanning later on.
    //
    // 'numberOfMarkersToKeepForOplog' exists solely to maintain legacy behavior of
    // 'OplogTruncateMarkers'. It serves as the maximum number of truncate markers to keep before
    // reclaiming the oldest truncate markers.
    static CollectionTruncateMarkers::MarkersCreationMethod computeInitialCreationMethod(
        int64_t numRecords,
        int64_t dataSize,
        int64_t minBytesPerMarker,
        boost::optional<int64_t> numberOfMarkersToKeepForOplog = boost::none);

    /**
     * A collection iterator class meant to encapsulate how the collection is scanned/sampled. As
     * the initialisation step is only concerned about getting either the next element of the
     * collection or a random one, this allows the user to specify how to perform these steps. This
     * allows one for example to avoid yielding and use raw cursors or to use the query framework so
     * that yielding is performed and we don't affect server stability.
     *
     * If we were to use query framework scans here we would incur on a layering violation as the
     * storage layer shouldn't have to interact with the query (higher) layer in here.
     */
    class CollectionIterator {
    public:
        // Returns the next element in the collection. Behaviour is the same as performing a normal
        // collection scan.
        virtual boost::optional<std::pair<RecordId, BSONObj>> getNext() = 0;

        // Returns a random document from the collection.
        virtual boost::optional<std::pair<RecordId, BSONObj>> getNextRandom() = 0;

        virtual RecordStore* getRecordStore() const = 0;

        // Reset the iterator. This will recreate any internal cursors used by the class so that
        // calling getNext* will start from the beginning again.
        virtual void reset(OperationContext* opCtx) = 0;

        int64_t numRecords() const {
            return getRecordStore()->numRecords();
        }

        int64_t dataSize() const {
            return getRecordStore()->dataSize();
        }
    };

    // Creates the initial set of markers. This will decide whether to perform a collection scan or
    // sampling based on the size of the collection.
    //
    // 'numberOfMarkersToKeepForOplog' exists solely to maintain legacy behavior of
    // 'OplogTruncateMarkers'. It serves as the maximum number of truncate markers to keep before
    // reclaiming the oldest truncate markers.
    static InitialSetOfMarkers createFromCollectionIterator(
        OperationContext* opCtx,
        CollectionIterator& collIterator,
        int64_t minBytesPerMarker,
        std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime,
        boost::optional<int64_t> numberOfMarkersToKeepForOplog = boost::none);

    // Creates the initial set of markers by fully scanning the collection. The set of markers
    // returned will have correct metrics.
    static InitialSetOfMarkers createMarkersByScanning(
        OperationContext* opCtx,
        CollectionIterator& collIterator,
        int64_t minBytesPerMarker,
        std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime);

    // Creates the initial set of markers by sampling the collection. The set of markers
    // returned will have approximate metrics. The metrics of each marker will be equal and contain
    // the collection's size and record count divided by the number of markers.
    static InitialSetOfMarkers createMarkersBySampling(
        OperationContext* opCtx,
        CollectionIterator& collIterator,
        int64_t estimatedRecordsPerMarker,
        int64_t estimatedBytesPerMarker,
        std::function<RecordIdAndWallTime(const Record&)> getRecordIdAndWallTime,
        TickSource* tickSource = globalSystemTickSource());

    void setMinBytesPerMarker(int64_t size);

    // Sets the _initialSamplingFinished variable to true. Allows other threads to know that initial
    // sampling of oplog truncate markers during startup has finished.
    void initialSamplingFinished();

    static constexpr uint64_t kRandomSamplesPerMarker = 10;

    Microseconds getCreationProcessingTime() const {
        return _totalTimeProcessing;
    }

    MarkersCreationMethod getMarkersCreationMethod() const {
        return _creationMethod;
    }

    //
    // The following methods are public only for use in tests.
    //

    size_t numMarkers_forTest() const {
        stdx::lock_guard<stdx::mutex> lk(_markersMutex);
        return _markers.size();
    }

    int64_t currentBytes_forTest() const {
        return _currentBytes.load();
    }

    int64_t currentRecords_forTest() const {
        return _currentRecords.load();
    }

    std::deque<Marker> getMarkers_forTest() const {
        // Return a copy of the vector.
        return _markers;
    }

private:
    friend class CollectionTruncateMarkersWithPartialExpiration;

    // Used to decide whether the oldest marker has expired. Implementations are free to use
    // whichever process they want to discern if there are expired markers.
    // This method will get called holding the _markersMutex.
    virtual bool _hasExcessMarkers(OperationContext* opCtx) const = 0;

    // Method used to notify the implementation of a new marker being created. Implementations are
    // free to implement this however they see fit by overriding it. By default this is a no-op.
    virtual void _notifyNewMarkerCreation() {};

    // Minimum number of bytes the marker being filled should contain before it gets added to the
    // deque of collection markers.
    AtomicWord<int64_t> _minBytesPerMarker;

    AtomicWord<int64_t> _currentRecords;  // Number of records in the marker being filled.
    AtomicWord<int64_t> _currentBytes;    // Number of bytes in the marker being filled.

    // Protects against concurrent access to the deque of collection markers and the
    // _initialSamplingFinished variable.
    mutable stdx::mutex _markersMutex;
    std::deque<Marker> _markers;  // front = oldest, back = newest.

    // Whether or not the initial set of markers has finished being sampled.
    bool _initialSamplingFinished = false;

protected:
    struct PartialMarkerMetrics {
        AtomicWord<int64_t>* currentRecords;
        AtomicWord<int64_t>* currentBytes;
    };

    template <typename F>
    auto modifyMarkersWith(F&& f) {
        static_assert(std::is_invocable_v<F, std::deque<Marker>&>,
                      "Function must be of type T(std::deque<Marker>&)");
        stdx::lock_guard lk(_markersMutex);
        return f(_markers);
    }

    template <typename F>
    auto checkMarkersWith(F&& f) const {
        static_assert(std::is_invocable_v<F, const std::deque<Marker>&>,
                      "Function must be of type T(const std::deque<Marker>&)");
        stdx::lock_guard lk(_markersMutex);
        return f(_markers);
    }

    const std::deque<Marker>& getMarkers() const {
        return _markers;
    }

    /**
     * Returns whether the truncate markers instance has no markers, whether partial or whole. Note
     * that this method can provide a stale result unless the caller can guarantee that no more
     * markers will be created.
     */
    bool isEmpty() const {
        stdx::lock_guard<stdx::mutex> lk(_markersMutex);
        return _markers.size() == 0 && _currentBytes.load() == 0 && _currentRecords.load() == 0;
    }

    Marker& createNewMarker(const RecordId& lastRecord, Date_t wallTime);

    template <typename F>
    auto modifyPartialMarker(F&& f) {
        static_assert(std::is_invocable_v<F, PartialMarkerMetrics>,
                      "Function must be of type T(PartialMarkerMetrics)");
        PartialMarkerMetrics metrics{&_currentRecords, &_currentBytes};
        return f(metrics);
    }

    // Amount of time spent scanning and/or sampling the collection during start up, if any.
    Microseconds _totalTimeProcessing;
    CollectionTruncateMarkers::MarkersCreationMethod _creationMethod;
};

/**
 * An extension of 'CollectionTruncateMarkers' that supports transforming the 'partial marker', when
 * it spans expired data, into a full marker without the 'minBytesPerMarker' size requirement.
 *
 * This is useful in time-based expiration systems as there could be low activity collections
 * containing expired data that can't be removed until covered by a full marker.
 */
class CollectionTruncateMarkersWithPartialExpiration : public CollectionTruncateMarkers {
public:
    /**
     * Partial marker expiration depends on tracking the highest seen RecordId and wall time
     * across the lifetime of the 'CollectionTruncateMarkersWithPartialExpiration' class.
     *
     * 'CollectionTruncateMarkersWithPartialExpiration' should always maintain a state where the
     * 'highestRecordId' and 'highestWallTime' are:
     *      . Greater than or equal to the 'lastRecord' and 'wall' of the most recent marker
     *      generated, if any.
     *      . Initialized provided records have been tracked at any point in time.
     *              * Records are tracked either by full markers, or a non-zero
     *                'leftoverRecordsCount' or 'leftoverRecordsBytes'.
     *      . Strictly increasing over time.
     *
     * Callers are responsible for ensuring the state requirements are met upon construction.
     */
    CollectionTruncateMarkersWithPartialExpiration(std::deque<Marker> markers,
                                                   RecordId highestRecordId,
                                                   Date_t highestWallTime,
                                                   int64_t leftoverRecordsCount,
                                                   int64_t leftoverRecordsBytes,
                                                   int64_t minBytesPerMarker,
                                                   Microseconds totalTimeSpentBuilding,
                                                   MarkersCreationMethod creationMethod)
        : CollectionTruncateMarkers(std::move(markers),
                                    leftoverRecordsCount,
                                    leftoverRecordsBytes,
                                    minBytesPerMarker,
                                    totalTimeSpentBuilding,
                                    creationMethod),
          _highestRecordId(std::move(highestRecordId)),
          _highestWallTime(highestWallTime) {}

    // Creates a partially filled marker if necessary. The criteria used is whether there is data in
    // the partial marker and whether the implementation's '_hasPartialMarkerExpired' returns true.
    void createPartialMarkerIfNecessary(OperationContext* opCtx);

    void updateCurrentMarkerAfterInsertOnCommit(OperationContext* opCtx,
                                                int64_t bytesInserted,
                                                const RecordId& highestInsertedRecordId,
                                                Date_t wallTime,
                                                int64_t countInserted) final;

    std::pair<const RecordId&, const Date_t&> getHighestRecordMetrics_forTest() const {
        return {_highestRecordId, _highestWallTime};
    }

private:
    // Highest marker seen during the lifetime of the class. Modifications must happen
    // while holding '_highestRecordMutex'.
    mutable stdx::mutex _highestRecordMutex;
    RecordId _highestRecordId;
    Date_t _highestWallTime;

    // Used to decide if the current partially built marker has expired.
    virtual bool _hasPartialMarkerExpired(OperationContext* opCtx,
                                          const RecordId& highestSeenRecordId,
                                          const Date_t& highestSeenWallTime) const {
        return false;
    }

    // Updates the highest seen RecordId and wall time if they are above the current ones.
    void _updateHighestSeenRecordIdAndWallTime(const RecordId& rId, Date_t wallTime);

protected:
    template <typename F>
    auto checkPartialMarkerWith(F&& fn) const {
        static_assert(std::is_invocable_v<F, const RecordId&, const Date_t&>,
                      "fn must be a callable of type T(const RecordId&, const Date_t&)");
        stdx::unique_lock lk(_highestRecordMutex);
        return fn(_highestRecordId, _highestWallTime);
    }

    void updateCurrentMarker(int64_t bytesAdded,
                             const RecordId& highestRecordId,
                             Date_t highestWallTime,
                             int64_t numRecordsAdded);
};

/**
 * A Collection iterator meant to work with raw RecordStores. This iterator will not yield between
 * calls to getNext()/getNextRandom().
 *
 * It is only safe to use when the user is not accepting any user operation. Some examples of when
 * this class can be used are during oplog initialisation, repair, recovery, etc.
 */
class UnyieldableCollectionIterator : public CollectionTruncateMarkers::CollectionIterator {
public:
    UnyieldableCollectionIterator(OperationContext* opCtx, RecordStore* rs) : _rs(rs) {
        reset(opCtx);
    }

    boost::optional<std::pair<RecordId, BSONObj>> getNext() final {
        auto record = _directionalCursor->next();
        if (!record) {
            return boost::none;
        }
        return std::make_pair(std::move(record->id), record->data.releaseToBson());
    }

    boost::optional<std::pair<RecordId, BSONObj>> getNextRandom() final {
        auto record = _randomCursor->next();
        if (!record) {
            return boost::none;
        }
        return std::make_pair(std::move(record->id), record->data.releaseToBson());
    }

    RecordStore* getRecordStore() const final {
        return _rs;
    }

    void reset(OperationContext* opCtx) final {
        _directionalCursor = _rs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
        _randomCursor = _rs->getRandomCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    }

private:
    RecordStore* _rs;
    std::unique_ptr<RecordCursor> _directionalCursor;
    std::unique_ptr<RecordCursor> _randomCursor;
};

}  // namespace mongo
