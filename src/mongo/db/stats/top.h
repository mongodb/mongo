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
#include "mongo/rpc/message.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <span>

#include <boost/date_time/posix_time/posix_time.hpp>

namespace mongo {

/**
 * Tracks cumulative latency statistics for a Service (shard-role or router-role).
 */
class ServiceLatencyTracker {
public:
    static ServiceLatencyTracker& getDecoration(Service* service);

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
class Top {
public:
    struct UsageData {
        long long time{0};
        long long count{0};

        void inc(long long micros) {
            count++;
            time += micros;
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

        OperationLatencyHistogram opLatencyHistogram;

        bool isStatsRecordingAllowed{true};
    };

    enum class LockType {
        ReadLocked,
        WriteLocked,
        NotLocked,
    };

    typedef StringMap<CollectionData> UsageMap;

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
    void appendStatsEntry(BSONObjBuilder& b, StringData name, const UsageData& data);

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
    // _lockUsage should always be acquired before using _usage.
    stdx::mutex _lockUsage;
    UsageMap _usage;
};

}  // namespace mongo
