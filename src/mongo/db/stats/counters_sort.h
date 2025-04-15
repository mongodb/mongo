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

namespace mongo {

class SortCounters {
public:
    void incrementSortCountersPerQuery(int64_t bytesSorted, int64_t keysSorted) {
        sortTotalBytesCounter.incrementRelaxed(bytesSorted);
        sortTotalKeysCounter.incrementRelaxed(keysSorted);
    }

    void incrementSortCountersPerSpilling(int64_t sortSpills, int64_t sortSpillBytes) {
        sortSpillsCounter.incrementRelaxed(sortSpills);
        sortSpillBytesCounter.incrementRelaxed(sortSpillBytes);
    }

    // Counters tracking sort stats across all engines
    // The total number of spills from sort stages
    Counter64& sortSpillsCounter = *MetricBuilder<Counter64>{"query.sort.spillToDisk"};
    // The total bytes spilled. This is the storage size after compression.
    Counter64& sortSpillBytesCounter = *MetricBuilder<Counter64>{"query.sort.spillToDiskBytes"};
    // The number of keys that we've sorted.
    Counter64& sortTotalKeysCounter = *MetricBuilder<Counter64>{"query.sort.totalKeysSorted"};
    // The amount of data we've sorted in bytes
    Counter64& sortTotalBytesCounter = *MetricBuilder<Counter64>{"query.sort.totalBytesSorted"};
};
extern SortCounters sortCounters;

}  // namespace mongo
