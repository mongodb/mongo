/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

/**
 * DB usage monitor.
 */

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/operation_latency_histogram.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string_view>

#include <boost/date_time/posix_time/posix_time.hpp>

namespace mongo {

/**
 * Tracks cumulative latency statistics for a Service (shard-role or router-role).
 */
class [[MONGO_MOD_PUBLIC]] ServiceLatencyTracker {
public:
    static ServiceLatencyTracker& getDecoration(Service* service);

    ServiceLatencyTracker();

    /**
     * Increments the cumulative histograms only if the operation came from a user.
     */
    void increment(OperationContext* opCtx,
                   Microseconds latency,
                   Microseconds workingTime,
                   Command::ReadWriteType readWriteType);
    /**
     * Increments the transactions histogram.
     */
    void incrementForTransaction(OperationContext* opCtx, Microseconds latency);


    /**
     * Appends the cumulative latency statistics for this service.
     */
    void appendTotalTimeStats(bool includeHistograms,
                              bool slowMSBucketsOnly,
                              BSONObjBuilder* builder);

    /**
     * Appends the cumulative working time statistics for this service.
     */
    void appendWorkingTimeStats(bool includeHistograms,
                                bool slowMSBucketsOnly,
                                BSONObjBuilder* builder);


private:
    AtomicOperationLatencyHistogram _totalTime;
    AtomicOperationLatencyHistogram _workingTime;
};

/**
 * Tracks shard-role usage by collection.
 */
class [[MONGO_MOD_PUBLIC]] Top {
public:
    struct UsageData {
        Atomic<long long> time{0};
        Atomic<long long> count{0};

        void inc(long long micros) {
            count.fetchAndAddRelaxed(1);
            time.fetchAndAddRelaxed(micros);
        }
    };

    struct CollectionData {
        UsageData total;

        UsageData readLock;
        UsageData writeLock;

        UsageData queries;
        UsageData getmore;
        UsageData insert;
        UsageData update;
        UsageData remove;
        UsageData commands;

        AtomicOperationLatencyHistogram opLatencyHistogram;

        // Sticky: once any op sets this false, it stays false. See updateCollectionData()
        // for why a relaxed read-then-conditional-store is safe.
        Atomic<bool> isStatsRecordingAllowed{true};
    };

    enum class LockType {
        ReadLocked,
        WriteLocked,
        NotLocked,
    };

    // CollectionData stores Atomic<T>s, which are non-movable. Use unique_ptr so the
    // CollectionData heap allocation is stable across map rehashes (only the unique_ptr
    // itself moves, not the atomics inside).
    typedef StringMap<std::unique_ptr<CollectionData>> UsageMap;

    Top() {
        ObservableMutexRegistry::get().add("topLockUsage", _lockUsage);
    }

    static Top& getDecoration(OperationContext* opCtx);

    void record(OperationContext* opCtx,
                const NamespaceString& nss,
                LogicalOp logicalOp,
                LockType lockType,
                Microseconds micros,
                bool command,
                Command::ReadWriteType readWriteType);

    /**
     * Same as the above, but for multiple namespaces.
     */
    void record(OperationContext* opCtx,
                std::span<const NamespaceString> nssSet,
                LogicalOp logicalOp,
                LockType lockType,
                Microseconds micros,
                bool command,
                Command::ReadWriteType readWriteType);

    /**
     * Adds the usage stats (time, count) for "name" to builder object "b".
     */
    void appendStatsEntry(BSONObjBuilder& b, std::string_view name, const UsageData& data);

    /**
     * Adds usage stats for "coll" onto builder object "result".
     */
    void appendUsageStatsForCollection(BSONObjBuilder& result, const CollectionData& coll);

    /**
     * Appends usage statistics for all collections.
     */
    void append(BSONObjBuilder& topStatsBuilder);

    void collectionDropped(const NamespaceString& nss);

    /**
     * Appends the collection-level latency statistics. Used as part of $collStats and only relevant
     * in the shard role.
     */
    void appendLatencyStats(const NamespaceString& nss,
                            bool includeHistograms,
                            BSONObjBuilder* builder);

    /**
     * Append the collection-level usage statistics.
     */
    void appendOperationStats(const NamespaceString& nss, BSONObjBuilder* builder);

private:
    // Runs `fn(collectionData)` for the map entry keyed by `key`, taking a shared lock on
    // the fast path and falling back to an exclusive lock to insert a new entry if missing.
    // Always-inlined because this sits on the Top::record hot path; the lambda body must
    // collapse into the caller so the shared_lock acquire/release fuses with the find()
    // and updateCollectionData().
    template <typename KeyT, typename Fn>
    MONGO_COMPILER_ALWAYS_INLINE void _withCollectionData(const KeyT& key, Fn&& fn) {
        {
            std::shared_lock lk(_lockUsage);  // NOLINT
            auto it = _usage.find(key);
            if (it != _usage.end()) {
                fn(*it->second);
                return;
            }
        }
        std::lock_guard lk(_lockUsage);
        auto& entry = _usage[key];
        if (!entry) {
            entry = std::make_unique<CollectionData>();
        }
        fn(*entry);
    }

    // _lockUsage protects the _usage map structure. Shared lock for reads and updates
    // to existing entries (atomic fields handle field-level safety). Exclusive lock
    // only for inserting new collections or erasing entries.
    ObservableSharedMutex _lockUsage;
    UsageMap _usage;
};

}  // namespace mongo
