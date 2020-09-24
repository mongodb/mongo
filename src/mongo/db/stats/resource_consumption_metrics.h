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
#include "mongo/platform/mutex.h"

namespace mongo {

/**
 * ResourceConsumption maintains thread-safe access into a map of resource consumption Metrics.
 */
class ResourceConsumption {
public:
    ResourceConsumption();

    static ResourceConsumption& get(OperationContext* opCtx);
    static ResourceConsumption& get(ServiceContext* svcCtx);

    struct ReadMetrics {
        void add(const ReadMetrics& other) {
            docBytesRead += other.docBytesRead;
            docUnitsRead += other.docUnitsRead;
            idxEntriesRead += other.idxEntriesRead;
            keysSorted += other.keysSorted;
        }

        ReadMetrics& operator+=(const ReadMetrics& other) {
            add(other);
            return *this;
        }

        // Number of document bytes read
        uint64_t docBytesRead;
        // Number of document units read
        uint64_t docUnitsRead;
        // Number of index entries read
        uint64_t idxEntriesRead;
        // Number of keys sorted for query operations
        uint64_t keysSorted;
    };

    /**
     * Metrics maintains a set of resource consumption metrics.
     */
    class Metrics {
    public:
        /**
         * Adds other Metrics to this one.
         */
        void add(const Metrics& other) {
            primaryMetrics += other.primaryMetrics;
            secondaryMetrics += other.secondaryMetrics;
            cpuMillis += other.cpuMillis;
            docBytesWritten += other.docBytesWritten;
            docUnitsWritten += other.docUnitsWritten;
            docUnitsReturned += other.docUnitsReturned;
        };

        Metrics& operator+=(const Metrics& other) {
            add(other);
            return *this;
        }

        // Read metrics recorded for queries processed while this node was primary
        ReadMetrics primaryMetrics;
        // Read metrics recorded for queries processed while this node was secondary
        ReadMetrics secondaryMetrics;
        // Amount of CPU time consumed by an operation in milliseconds
        uint64_t cpuMillis;
        // Number of document bytes written
        uint64_t docBytesWritten;
        // Number of document units written
        uint64_t docUnitsWritten;
        // Number of document units returned by a query.
        uint64_t docUnitsReturned;

        void toBson(BSONObjBuilder* builder) const;
    };

    /**
     * MetricsCollector maintains non-thread-safe, per-operation resource consumption metrics for a
     * specific database.
     */
    class MetricsCollector {
    public:
        static MetricsCollector& get(OperationContext* opCtx);

        /**
         * When called, resource consumption metrics should be recorded and added to the global
         * structure.
         */
        void beginScopedCollecting() {
            invariant(!isInScope());
            _collecting = ScopedCollectionState::kInScopeCollecting;
        }

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
        bool endScopedCollecting() {
            bool wasCollecting = isCollecting();
            _collecting = ScopedCollectionState::kInactive;
            return wasCollecting;
        }

        bool isCollecting() const {
            return _collecting == ScopedCollectionState::kInScopeCollecting;
        }

        bool isInScope() const {
            return _collecting == ScopedCollectionState::kInScopeCollecting ||
                _collecting == ScopedCollectionState::kInScopeNotCollecting;
        }

        /**
         * Set the database name associated with these metrics.
         */
        void setDbName(const std::string& dbName) {
            _dbName = dbName;
        }

        const std::string& getDbName() const {
            return _dbName;
        }

        /**
         * To observe the stored Metrics, the dbName must be set. This prevents "losing" collected
         * Metrics due to the Collector stopping without being associated with any database yet.
         */
        Metrics& getMetrics() {
            invariant(!_dbName.empty(), "observing Metrics before a dbName has been set");
            return _metrics;
        }

        const Metrics& getMetrics() const {
            invariant(!_dbName.empty(), "observing Metrics before a dbName has been set");
            return _metrics;
        }

    private:
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
        std::string _dbName;
        Metrics _metrics;
    };

    /**
     * When instantiated with commandCollectsMetrics=true, enables operation resource consumption
     * collection. When destructed, appends collected metrics to the global structure.
     */
    class ScopedMetricsCollector {
    public:
        ScopedMetricsCollector(OperationContext* opCtx, bool commandCollectsMetrics);
        ScopedMetricsCollector(OperationContext* opCtx) : ScopedMetricsCollector(opCtx, true) {}
        ~ScopedMetricsCollector();

    private:
        bool _topLevel;
        OperationContext* _opCtx;
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
     * Returns true if resource consumption metrics should be aggregated globally.
     */
    static bool isMetricsAggregationEnabled();

    /**
     * Adds a MetricsCollector's Metrics to an existing Metrics object in the map, keyed by
     * database name. If no Metrics exist for the database, the value is initialized with the
     * provided MetricsCollector's Metrics.
     *
     * The MetricsCollector's database name must not be an empty string.
     */
    void add(const MetricsCollector& metrics);

    /**
     * Returns a copy of the Metrics map.
     */
    using MetricsMap = std::map<std::string, Metrics>;
    MetricsMap getMetrics() const;

    /**
     * Returns the Metrics map and then clears the contents. This attempts to swap and return the
     * metrics map rather than making a full copy like getMetrics.
     */
    MetricsMap getAndClearMetrics();

private:
    // Protects _metrics
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ResourceConsumption::_mutex");
    MetricsMap _metrics;
};

}  // namespace mongo
