// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/spilling/spilling_stats.h"

#include "mongo/logv2/log.h"

#include <string_view>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo {

inline void addWithOverflowCheck(uint64_t additionalValue,
                                 std::string_view statName,
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
