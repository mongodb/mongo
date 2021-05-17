// counters.h

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

#include "mongo/base/counter.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/message.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/with_alignment.h"

namespace mongo {

/**
 * for storing operation counters
 * note: not thread safe.  ok with that for speed
 */
class OpCounters {
public:
    OpCounters();
    void gotInserts(int n);
    void gotInsert();
    void gotQuery();
    void gotUpdate();
    void gotDelete();
    void gotGetMore();
    void gotCommand();

    void gotInsertsDeprecated(int n);
    void gotQueryDeprecated();
    void gotUpdateDeprecated();
    void gotDeleteDeprecated();
    void gotGetMoreDeprecated();
    void gotKillCursorsDeprecated();

    BSONObj getObj() const;

    // thse are used by snmp, and other things, do not remove
    const AtomicUInt32* getInsert() const {
        return &_insert;
    }
    const AtomicUInt32* getQuery() const {
        return &_query;
    }
    const AtomicUInt32* getUpdate() const {
        return &_update;
    }
    const AtomicUInt32* getDelete() const {
        return &_delete;
    }
    const AtomicUInt32* getGetMore() const {
        return &_getmore;
    }
    const AtomicUInt32* getCommand() const {
        return &_command;
    }

private:
    void _checkWrap();

    CacheAligned<AtomicUInt32> _insert;
    CacheAligned<AtomicUInt32> _query;
    CacheAligned<AtomicUInt32> _update;
    CacheAligned<AtomicUInt32> _delete;
    CacheAligned<AtomicUInt32> _getmore;
    CacheAligned<AtomicUInt32> _command;

    // Counters for deprecated opcodes.
    CacheAligned<AtomicUInt32> _insertDeprecated;
    CacheAligned<AtomicUInt32> _queryDeprecated;
    CacheAligned<AtomicUInt32> _updateDeprecated;
    CacheAligned<AtomicUInt32> _deleteDeprecated;
    CacheAligned<AtomicUInt32> _getmoreDeprecated;
    CacheAligned<AtomicUInt32> _killcursorsDeprecated;
};

extern OpCounters globalOpCounters;
extern OpCounters replOpCounters;

class NetworkCounter {
public:
    // Increment the counters for the number of bytes read directly off the wire
    void hitPhysicalIn(long long bytes);
    void hitPhysicalOut(long long bytes);

    // Increment the counters for the number of bytes passed out of the TransportLayer to the
    // server
    void hitLogicalIn(long long bytes);
    void hitLogicalOut(long long bytes);

    void append(BSONObjBuilder& b);

private:
    CacheAligned<AtomicInt64> _physicalBytesIn{0};
    CacheAligned<AtomicInt64> _physicalBytesOut{0};

    // These two counters are always incremented at the same time, so
    // we place them on the same cache line.
    struct Together {
        AtomicInt64 logicalBytesIn{0};
        AtomicInt64 requests{0};
    };
    CacheAligned<Together> _together{};
    static_assert(sizeof(decltype(_together)) <= stdx::hardware_constructive_interference_size,
                  "cache line spill");

    CacheAligned<AtomicInt64> _logicalBytesOut{0};
};

extern NetworkCounter networkCounter;

class AggStageCounters {
public:
    // Container for a stage count metric along with its corresponding counter.
    struct StageCounter {
        explicit StageCounter(const std::string& name)
            : metric("aggStageCounters." + name, &counter) {}

        Counter64 counter;
        ServerStatusMetricField<Counter64> metric;
    };

    // Map of aggregation stages to the number of occurrences.
    stdx::unordered_map<std::string, std::unique_ptr<StageCounter>> stageCounterMap;
};

extern AggStageCounters aggStageCounters;
}  // namespace mongo
