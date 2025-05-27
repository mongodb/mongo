/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/spilling/spilling_stats.h"

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <limits>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo {

inline void addWithOverflowCheck(uint64_t additionalValue,
                                 StringData statName,
                                 std::once_flag& warnForOverflow,
                                 uint64_t& currentValue) {
    uint64_t totalValue = 0;
    bool hasOverflown = overflow::add(currentValue, additionalValue, &totalValue);
    if (MONGO_unlikely(hasOverflown)) {
        std::call_once(warnForOverflow, [&]() {
            LOGV2_WARNING(
                9942400,
                "Increasing spilling metric further will cause an overflow. Skipping increase.",
                "metric name"_attr = statName,
                "current value"_attr = currentValue,
                "additional value"_attr = additionalValue);
        });
    } else {
        currentValue = totalValue;
    }
}

void SpillingStats::incrementSpills(uint64_t spills) {
    static std::once_flag spillsWarnForOverflow;
    addWithOverflowCheck(spills, "spills", spillsWarnForOverflow, _spills);
}

void SpillingStats::incrementSpilledBytes(uint64_t spilledBytes) {
    static std::once_flag spilledBytesWarnForOverflow;
    addWithOverflowCheck(spilledBytes, "spilledBytes", spilledBytesWarnForOverflow, _spilledBytes);
}

void SpillingStats::incrementSpilledDataStorageSize(uint64_t spilledDataStorageSize) {
    static std::once_flag spilledDataStorageSizeWarnForOverflow;
    addWithOverflowCheck(spilledDataStorageSize,
                         "spilledDataStorageSize",
                         spilledDataStorageSizeWarnForOverflow,
                         _spilledDataStorageSize);
}

void SpillingStats::incrementSpilledRecords(uint64_t spilledRecords) {
    static std::once_flag spilledRecordsWarnForOverflow;
    addWithOverflowCheck(
        spilledRecords, "spilledRecords", spilledRecordsWarnForOverflow, _spilledRecords);
}
}  // namespace mongo
