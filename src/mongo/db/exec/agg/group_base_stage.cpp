// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/group_base_stage.h"

namespace mongo::exec::agg {

Document GroupBaseStage::getExplainOutput(const query_shape::SerializationOptions& opts) const {

    MutableDocument accumulatorMemUsage;
    const auto& accumulatedFields = _groupProcessor->getAccumulationStatements();
    const auto& memoryTracker = _groupProcessor->getMemoryTracker();
    for (size_t i = 0; i < accumulatedFields.size(); i++) {
        accumulatorMemUsage[opts.serializeFieldPathFromString(accumulatedFields[i].fieldName)] =
            opts.serializeLiteral(static_cast<long long>(
                memoryTracker.peakTrackedMemoryBytes(accumulatedFields[i].fieldName)));
    }

    MutableDocument result(Stage::getExplainOutput(opts));
    result["maxAccumulatorMemoryUsageBytes"] = Value(accumulatorMemUsage.freezeToValue());

    const auto& stats = _groupProcessor->getStats();
    result["totalOutputDataSizeBytes"] =
        opts.serializeLiteral(static_cast<long long>(stats.totalOutputDataSizeBytes));
    result["usedDisk"] = opts.serializeLiteral(stats.spillingStats.getSpills() > 0);
    result["spills"] =
        opts.serializeLiteral(static_cast<long long>(stats.spillingStats.getSpills()));
    result["spilledDataStorageSize"] = opts.serializeLiteral(
        static_cast<long long>(stats.spillingStats.getSpilledDataStorageSize()));
    result["spilledBytes"] =
        opts.serializeLiteral(static_cast<long long>(stats.spillingStats.getSpilledBytes()));
    result["spilledRecords"] =
        opts.serializeLiteral(static_cast<long long>(stats.spillingStats.getSpilledRecords()));
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        result["peakTrackedMemBytes"] =
            opts.serializeLiteral(static_cast<long long>(stats.peakTrackedMemBytes));
    }

    return result.freeze();
}

}  // namespace mongo::exec::agg
