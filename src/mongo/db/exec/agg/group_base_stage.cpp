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

#include "mongo/db/exec/agg/group_base_stage.h"

namespace mongo::exec::agg {

Document GroupBaseStage::getExplainOutput(const SerializationOptions& opts) const {

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
