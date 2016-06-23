// top.h : DB usage monitor.

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/date_time/posix_time/posix_time.hpp>

#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/operation_latency_histogram.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/net/message.h"
#include "mongo/util/string_map.h"

namespace mongo {

class ServiceContext;

/**
 * tracks usage by collection
 */
class Top {
public:
    static Top& get(ServiceContext* service);

    Top() = default;

    struct UsageData {
        UsageData() : time(0), count(0) {}
        UsageData(const UsageData& older, const UsageData& newer);
        long long time;
        long long count;

        void inc(long long micros) {
            count++;
            time += micros;
        }
    };

    struct CollectionData {
        /**
         * constructs a diff
         */
        CollectionData() {}
        CollectionData(const CollectionData& older, const CollectionData& newer);

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
    };

    typedef StringMap<CollectionData> UsageMap;

public:
    void record(OperationContext* txn,
                StringData ns,
                LogicalOp logicalOp,
                int lockType,
                long long micros,
                bool command,
                Command::ReadWriteType readWriteType);

    void append(BSONObjBuilder& b);

    void cloneMap(UsageMap& out) const;

    void collectionDropped(StringData ns);

    /**
     * Appends the collection-level latency statistics
     */
    void appendLatencyStats(StringData ns, BSONObjBuilder* builder);

    /**
     * Increments the global histogram.
     */
    void incrementGlobalLatencyStats(OperationContext* txn,
                                     uint64_t latency,
                                     Command::ReadWriteType readWriteType);

    /**
     * Appends the global latency statistics.
     */
    void appendGlobalLatencyStats(BSONObjBuilder* builder);

private:
    void _appendToUsageMap(BSONObjBuilder& b, const UsageMap& map) const;

    void _appendStatsEntry(BSONObjBuilder& b, const char* statsName, const UsageData& map) const;

    void _record(OperationContext* txn,
                 CollectionData& c,
                 LogicalOp logicalOp,
                 int lockType,
                 long long micros,
                 Command::ReadWriteType readWriteType);

    void _incrementHistogram(OperationContext* txn,
                             long long latency,
                             OperationLatencyHistogram* histogram,
                             Command::ReadWriteType readWriteType);

    mutable SimpleMutex _lock;
    OperationLatencyHistogram _globalHistogramStats;
    UsageMap _usage;
    std::string _lastDropped;
};

}  // namespace mongo
