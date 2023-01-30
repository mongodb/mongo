/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <map>
#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_cpu_timer.h"
#include "mongo/platform/mutex.h"

namespace mongo {

/**
 * ResourceConsumption maintains thread-safe access into a map of resource consumption Metrics.
 */
class ResourceConsumption {
public:
    ResourceConsumption() = default;

    static ResourceConsumption& get(OperationContext* opCtx);
    static ResourceConsumption& get(ServiceContext* svcCtx);

    /**
     * UnitCounter observes individual input datums and then calculates the total number of bytes
     * and whole number units observed.
     */
    class UnitCounter {
    public:
        UnitCounter() = default;

        void add(const UnitCounter& other) {
            _bytes += other._bytes;
            _units += other._units;
        }

        UnitCounter& operator+=(const UnitCounter& other) {
            add(other);
            return *this;
        }

        long long bytes() const {
            return _bytes;
        }
        long long units() const {
            return _units;
        }

        /**
         * Call once per input datum with its size in bytes.
         *
         * This function calculates the number of units observed based on the implentation-specific
         * unitSize(). The function uses the following formula to calculate the number of units per
         * datum:
         *
         * units = ceil (datum bytes / unit size in bytes)
         *
         * This achieves the goal of counting small datums as at least one unit while ensuring
         * larger units are accounted proportionately. This can result in overstating smaller datums
         * when the unit size is large. This is desired behavior, and the extent to which small
         * datums are overstated is tunable by the unit size of the implementor.
         */
        void observeOne(size_t datumBytes);

    protected:
        /**
         * Returns the implementation-specific unit size.
         */
        virtual int unitSize() const = 0;

        long long _bytes = 0;
        long long _units = 0;
    };

    /** DocumentUnitCounter records the number of document units observed. */
    class DocumentUnitCounter : public UnitCounter {
    private:
        int unitSize() const final;
    };

    /** IdxEntryUnitCounter records the number of index entry units observed. */
    class IdxEntryUnitCounter : public UnitCounter {
    private:
        int unitSize() const final;
    };

    /** TotalUnitWriteCounter records the number of units of document plus associated indexes
     * observed. */
    class TotalUnitWriteCounter {
    public:
        void observeOneDocument(size_t datumBytes);
        void observeOneIndexEntry(size_t datumBytes);

        TotalUnitWriteCounter& operator+=(TotalUnitWriteCounter other) {
            // Flush the accumulators, in case there is anything still pending.
            other.observeOneDocument(0);
            observeOneDocument(0);
            _units += other._units;
            return *this;
        }

        long long units() const {
            // Flush the accumulators, in case there is anything still pending.
            TotalUnitWriteCounter copy(*this);
            copy.observeOneDocument(0);
            return copy._units;
        }

    private:
        int unitSize() const;
        long long _accumulatedDocumentBytes = 0;
        long long _accumulatedIndexBytes = 0;
        long long _units = 0;
    };

    /** ReadMetrics maintains metrics for read operations. */
    class ReadMetrics {
    public:
        ReadMetrics() = default;

        void add(const ReadMetrics& other) {
            docsRead += other.docsRead;
            idxEntriesRead += other.idxEntriesRead;
            docsReturned += other.docsReturned;
            keysSorted += other.keysSorted;
            sorterSpills += other.sorterSpills;
            cursorSeeks += other.cursorSeeks;
        }

        ReadMetrics& operator+=(const ReadMetrics& other) {
            add(other);
            return *this;
        }

        /**
         * Reports all metrics on a BSONObjBuilder.
         */
        void toBson(BSONObjBuilder* builder) const;

        // Number of document units read
        DocumentUnitCounter docsRead;
        // Number of index entry units read
        IdxEntryUnitCounter idxEntriesRead;
        // Number of document units returned by a query
        DocumentUnitCounter docsReturned;

        // Number of keys sorted for query operations
        long long keysSorted = 0;
        // Number of individual spills of data to disk by the sorter
        long long sorterSpills = 0;
        // Number of cursor seeks
        long long cursorSeeks = 0;
    };

    /* WriteMetrics maintains metrics for write operations. */
    class WriteMetrics {
    public:
        void add(const WriteMetrics& other) {
            docsWritten += other.docsWritten;
            idxEntriesWritten += other.idxEntriesWritten;
            totalWritten += other.totalWritten;
        }

        WriteMetrics& operator+=(const WriteMetrics& other) {
            add(other);
            return *this;
        }

        /**
         * Reports all metrics on a BSONObjBuilder.
         */
        void toBson(BSONObjBuilder* builder) const;

        // Number of documents written
        DocumentUnitCounter docsWritten;
        // Number of index entries written
        IdxEntryUnitCounter idxEntriesWritten;
        // Number of total units written
        TotalUnitWriteCounter totalWritten;
    };

    /**
     * OperationMetrics maintains resource consumption metrics for a single operation.
     */
    class OperationMetrics {
    public:
        OperationMetrics() = default;

        /**
         * Reports all metrics on a BSONObjBuilder.
         */
        void toBson(BSONObjBuilder* builder) const;

        /**
         * Reports metrics on a BSONObjBuilder. Only non-zero fields are reported.
         */
        void toBsonNonZeroFields(BSONObjBuilder* builder) const;

        // Read and write metrics for this operation
        ReadMetrics readMetrics;
        WriteMetrics writeMetrics;

        // Records CPU time consumed by this operation.
        OperationCPUTimer* cpuTimer = nullptr;
    };

    /**
     * AggregatedMetrics maintains a structure of resource consumption metrics designed to be
     * aggregated and added together at some global level.
     */
    class AggregatedMetrics {
    public:
        void add(const AggregatedMetrics& other) {
            primaryReadMetrics += other.primaryReadMetrics;
            secondaryReadMetrics += other.secondaryReadMetrics;
            writeMetrics += other.writeMetrics;
            cpuNanos += other.cpuNanos;
        };

        AggregatedMetrics& operator+=(const AggregatedMetrics& other) {
            add(other);
            return *this;
        }

        /**
         * Reports all metrics on a BSONObjBuilder.
         */
        void toBson(BSONObjBuilder* builder) const;

        // Read metrics recorded for queries processed while this node was primary
        ReadMetrics primaryReadMetrics;

        // Read metrics recorded for queries processed while this node was secondary
        ReadMetrics secondaryReadMetrics;

        // Write metrics recorded for all operations
        WriteMetrics writeMetrics;

        // Amount of CPU time consumed by an operation in nanoseconds
        Nanoseconds cpuNanos;
    };

    /**
     * MetricsCollector maintains non-thread-safe, per-operation resource consumption metrics for a
     * specific database.
     */
    class MetricsCollector {
    public:
        MetricsCollector() = default;

        static MetricsCollector& get(OperationContext* opCtx);

        /**
         * When called, resource consumption metrics should be recorded for this operation.
         */
        void beginScopedCollecting(OperationContext* opCtx, const std::string& dbName);

        /**
         * When called, sets state that a ScopedMetricsCollector is in scope, but is not recording
         * metrics. This is to support nesting Scope objects and preventing lower levels from
         * overriding this behavior.
         */
        void beginScopedNotCollecting() {
            invariant(!isInScope());
            _collecting = ScopedCollectionState::kInScopeNotCollecting;
        }

        /**
         * When called, resource consumption metrics should not be recorded. Returns whether this
         * Collector was in a collecting state.
         */
        bool endScopedCollecting();

        bool isCollecting() const {
            return !_paused && _collecting == ScopedCollectionState::kInScopeCollecting;
        }

        bool isInScope() const {
            return _collecting == ScopedCollectionState::kInScopeCollecting ||
                _collecting == ScopedCollectionState::kInScopeNotCollecting;
        }

        /**
         * Returns whether or not a ScopedMetricsCollector is currently collecting or was collecting
         * metrics at any point for this operation.
         */
        bool hasCollectedMetrics() const {
            return _hasCollectedMetrics;
        }

        const std::string& getDbName() const {
            return _dbName;
        }

        /**
         * To observe the stored Metrics, the dbName must be set. This prevents "losing" collected
         * Metrics due to the Collector stopping without being associated with any database yet.
         */
        OperationMetrics& getMetrics() {
            invariant(!_dbName.empty(), "observing Metrics before a dbName has been set");
            return _metrics;
        }

        const OperationMetrics& getMetrics() const {
            invariant(!_dbName.empty(), "observing Metrics before a dbName has been set");
            return _metrics;
        }

        void reset() {
            invariant(!isInScope());
            *this = {};
        }

        /**
         * This should be called once per document read with the number of bytes read for that
         * document.  This is a no-op when metrics collection is disabled on this operation.
         */
        void incrementOneDocRead(StringData uri, size_t docBytesRead);

        /**
         * This should be called once per index entry read with the number of bytes read for that
         * entry. This is a no-op when metrics collection is disabled on this operation.
         */
        void incrementOneIdxEntryRead(StringData uri, size_t idxEntryBytesRead);

        /**
         * Increments the number of keys sorted for a query operation. This is a no-op when metrics
         * collection is disabled on this operation.
         */
        void incrementKeysSorted(size_t keysSorted);

        /**
         * Increments the number of number of individual spills to disk by the sorter for query
         * operations. This is a no-op when metrics collection is disabled on this operation.
         */
        void incrementSorterSpills(size_t spills);

        /**
         * Increments the number of document units returned in the command response.
         */
        void incrementDocUnitsReturned(StringData ns, DocumentUnitCounter docUnitsReturned);

        /**
         * This should be called once per document written with the number of bytes written for that
         * document. This is a no-op when metrics collection is disabled on this operation. This
         * function should not be called when the operation is a write to the oplog. The metrics are
         * only for operations that are not oplog writes.
         */
        void incrementOneDocWritten(StringData uri, size_t docBytesWritten);

        /**
         * This should be called once per index entry written with the number of bytes written for
         * that entry. This is a no-op when metrics collection is disabled on this operation.
         */
        void incrementOneIdxEntryWritten(StringData uri, size_t idxEntryBytesWritten);

        /**
         * This should be called once every time the storage engine successfully does a cursor seek.
         * Note that if it takes multiple attempts to do a successful seek, this function should
         * only be called once. If the seek does not find anything, this function should not be
         * called.
         */
        void incrementOneCursorSeek(StringData uri);

        /**
         * Pause metrics collection, overriding kInScopeCollecting status. The scope status may be
         * changed during a pause, but will not come into effect until resume() is called.
         */
        void pause() {
            invariant(!_paused);
            _paused = true;
        }

        /**
         * Resume metrics collection. Trying to resume a non-paused object will invariant.
         */
        void resume() {
            invariant(_paused);
            _paused = false;
        }

        /**
         * Returns if the current object is in paused state.
         */
        bool isPaused() {
            return _paused;
        }

    private:
        // Privatize copy constructors to prevent callers from accidentally copying when this is
        // decorated on the OperationContext by reference.
        MetricsCollector(const MetricsCollector&) = default;
        MetricsCollector& operator=(const MetricsCollector&) = default;

        /**
         * Helper function that calls the Func when this collector is currently collecting metrics.
         */
        template <typename Func>
        void _doIfCollecting(Func&& func);

        /**
         * Represents the ScopedMetricsCollector state.
         */
        enum class ScopedCollectionState {
            // No ScopedMetricsCollector is in scope
            kInactive,
            // A ScopedMetricsCollector is in scope but not collecting metrics
            kInScopeNotCollecting,
            // A ScopedMetricsCollector is in scope and collecting metrics
            kInScopeCollecting
        };
        ScopedCollectionState _collecting = ScopedCollectionState::kInactive;
        bool _hasCollectedMetrics = false;
        std::string _dbName;
        OperationMetrics _metrics;
        bool _paused = false;
    };

    /**
     * When instantiated with commandCollectsMetrics=true, enables operation resource consumption
     * collection. When destructed, appends collected metrics to the global structure, if metrics
     * aggregation is enabled.
     */
    class ScopedMetricsCollector {
    public:
        ScopedMetricsCollector(OperationContext* opCtx,
                               const std::string& dbName,
                               bool commandCollectsMetrics);
        ScopedMetricsCollector(OperationContext* opCtx, const std::string& dbName)
            : ScopedMetricsCollector(opCtx, dbName, true) {}
        ~ScopedMetricsCollector();

    private:
        bool _topLevel;
        OperationContext* _opCtx;
    };

    /**
     * RAII-style class to temporarily pause the MetricsCollector in the OperationContext. This
     * applies even if the MetricsCollector is started explicitly in lower levels.
     *
     * Exception: CPU metrics are not paused.
     */
    class PauseMetricsCollectorBlock {
        PauseMetricsCollectorBlock(const PauseMetricsCollectorBlock&) = delete;
        PauseMetricsCollectorBlock& operator=(const PauseMetricsCollectorBlock&) = delete;

    public:
        explicit PauseMetricsCollectorBlock(OperationContext* opCtx) : _opCtx(opCtx) {
            auto& metrics = MetricsCollector::get(_opCtx);
            _wasPaused = metrics.isPaused();
            if (!_wasPaused) {
                metrics.pause();
            }
        }

        ~PauseMetricsCollectorBlock() {
            if (!_wasPaused) {
                auto& metrics = MetricsCollector::get(_opCtx);
                metrics.resume();
            }
        }

    private:
        OperationContext* _opCtx;
        bool _wasPaused;
    };

    /**
     * Returns whether the database's metrics should be collected.
     */
    static bool shouldCollectMetricsForDatabase(StringData dbName) {
        if (dbName == NamespaceString::kAdminDb || dbName == NamespaceString::kConfigDb ||
            dbName == NamespaceString::kLocalDb) {
            return false;
        }
        return true;
    }

    /**
     * Returns true if resource consumption metrics should be collected per-operation.
     */
    static bool isMetricsCollectionEnabled();

    /**
     * Returns true if operations should profile resource consumption metrics.
     */
    static bool isMetricsProfilingEnabled();

    /**
     * Returns true if resource consumption metrics should be aggregated globally.
     */
    static bool isMetricsAggregationEnabled();

    /**
     * Merges OperationMetrics with a globally-aggregated structure. The OperationMetrics's contents
     * are added to existing values in a map keyed by database name. Read metrics will be attributed
     * to the current replication state. If no metrics already exist for the database, a new value
     * is initialized with the one provided.
     *
     * The database name must not be an empty string.
     */
    void merge(OperationContext* opCtx, const std::string& dbName, const OperationMetrics& metrics);

    /**
     * Returns a copy of the per-database metrics map.
     */
    using MetricsMap = std::map<std::string, AggregatedMetrics>;
    MetricsMap getDbMetrics() const;

    /**
     *  Returns the number of databases with aggregated metrics.
     */
    size_t getNumDbMetrics() const;

    /**
     * Returns the per-database metrics map and then clears the contents. This attempts to swap and
     * return the metrics map rather than making a full copy like getDbMetrics.
     */
    MetricsMap getAndClearDbMetrics();

    /**
     * Returns the globally-aggregated CPU time.
     */
    Nanoseconds getCpuTime() const;

    /**
     * Clears the existing CPU time.
     */
    Nanoseconds getAndClearCpuTime();

private:
    // Protects _dbMetrics and _cpuTime
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ResourceConsumption::_mutex");
    MetricsMap _dbMetrics;
    Nanoseconds _cpuTime;
};
}  // namespace mongo
