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

#include "mongo/db/admission/execution_control/execution_control_stats.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::admission::execution_control {

void AdmissionsHistogram::record(int32_t admissions) {
    if (admissions <= 0) {
        return;
    }
    size_t index = _getBucketIndex(admissions);
    _buckets[index].fetchAndAddRelaxed(1);
}

void AdmissionsHistogram::appendStats(BSONObjBuilder& b) const {
    for (size_t i = 0; i < kNumBuckets; ++i) {
        b.append(kBucketNames[i], _buckets[i].loadRelaxed());
    }
}

size_t AdmissionsHistogram::_getBucketIndex(int32_t admissions) {
    if (admissions <= 2) {
        return 0;
    }
    auto idx = static_cast<size_t>(std::ceil(std::log2(admissions))) - 1;
    return std::min(idx, static_cast<size_t>(kNumBuckets - 1));
}

DelinquencyStats::DelinquencyStats(int64_t totalDelinquentAcquisitions,
                                   int64_t totalAcquisitionDelinquencyMillis,
                                   int64_t maxAcquisitionDelinquencyMillis)
    : totalDelinquentAcquisitions(totalDelinquentAcquisitions),
      totalAcquisitionDelinquencyMillis(totalAcquisitionDelinquencyMillis),
      maxAcquisitionDelinquencyMillis(maxAcquisitionDelinquencyMillis) {}

DelinquencyStats::DelinquencyStats(const DelinquencyStats& other) {
    *this = other;
}

DelinquencyStats& DelinquencyStats::operator=(const DelinquencyStats& other) {
    totalDelinquentAcquisitions.storeRelaxed(other.totalDelinquentAcquisitions.loadRelaxed());
    totalAcquisitionDelinquencyMillis.storeRelaxed(
        other.totalAcquisitionDelinquencyMillis.loadRelaxed());
    maxAcquisitionDelinquencyMillis.storeRelaxed(
        other.maxAcquisitionDelinquencyMillis.loadRelaxed());
    return *this;
}

DelinquencyStats& DelinquencyStats::operator+=(const DelinquencyStats& other) {
    totalDelinquentAcquisitions.fetchAndAddRelaxed(other.totalDelinquentAcquisitions.loadRelaxed());
    totalAcquisitionDelinquencyMillis.fetchAndAddRelaxed(
        other.totalAcquisitionDelinquencyMillis.loadRelaxed());
    int64_t currentMax, newMax;
    do {
        currentMax = maxAcquisitionDelinquencyMillis.loadRelaxed();
        newMax = other.maxAcquisitionDelinquencyMillis.loadRelaxed();
    } while (newMax > currentMax &&
             !maxAcquisitionDelinquencyMillis.compareAndSwap(&currentMax, newMax));
    return *this;
}

void DelinquencyStats::appendStats(BSONObjBuilder& b) const {
    b.append("totalDelinquentAcquisitions", totalDelinquentAcquisitions.loadRelaxed());
    b.append("totalAcquisitionDelinquencyMillis", totalAcquisitionDelinquencyMillis.loadRelaxed());
    b.append("maxAcquisitionDelinquencyMillis", maxAcquisitionDelinquencyMillis.loadRelaxed());
}

OperationFinalizedStats::OperationFinalizedStats(const OperationFinalizedStats& other) {
    *this = other;
}

OperationFinalizedStats& OperationFinalizedStats::operator=(const OperationFinalizedStats& other) {
    totalCPUUsageMicros.storeRelaxed(other.totalCPUUsageMicros.loadRelaxed());
    totalElapsedTimeMicros.storeRelaxed(other.totalElapsedTimeMicros.loadRelaxed());
    totalOpsLoadShed.storeRelaxed(other.totalOpsLoadShed.loadRelaxed());
    totalCPUUsageLoadShed.storeRelaxed(other.totalCPUUsageLoadShed.loadRelaxed());
    totalElapsedTimeMicrosLoadShed.storeRelaxed(other.totalElapsedTimeMicrosLoadShed.loadRelaxed());
    totalAdmissionsLoadShed.storeRelaxed(other.totalAdmissionsLoadShed.loadRelaxed());
    totalQueuedTimeMicrosLoadShed.storeRelaxed(other.totalQueuedTimeMicrosLoadShed.loadRelaxed());
    return *this;
}

OperationFinalizedStats& OperationFinalizedStats::operator+=(const OperationFinalizedStats& other) {
    totalCPUUsageMicros.fetchAndAddRelaxed(other.totalCPUUsageMicros.loadRelaxed());
    totalElapsedTimeMicros.fetchAndAddRelaxed(other.totalElapsedTimeMicros.loadRelaxed());
    totalOpsLoadShed.fetchAndAddRelaxed(other.totalOpsLoadShed.loadRelaxed());
    totalCPUUsageLoadShed.fetchAndAddRelaxed(other.totalCPUUsageLoadShed.loadRelaxed());
    totalElapsedTimeMicrosLoadShed.fetchAndAddRelaxed(
        other.totalElapsedTimeMicrosLoadShed.loadRelaxed());
    totalAdmissionsLoadShed.fetchAndAddRelaxed(other.totalAdmissionsLoadShed.loadRelaxed());
    totalQueuedTimeMicrosLoadShed.fetchAndAddRelaxed(
        other.totalQueuedTimeMicrosLoadShed.loadRelaxed());
    return *this;
}

void OperationFinalizedStats::appendStats(BSONObjBuilder& b) const {
    b.append("totalCPUUsageMicros", totalCPUUsageMicros.loadRelaxed());
    b.append("totalElapsedTimeMicros", totalElapsedTimeMicros.loadRelaxed());
    b.append("totalOpsLoadShed", totalOpsLoadShed.loadRelaxed());
    b.append("totalCPUUsageLoadShed", totalCPUUsageLoadShed.loadRelaxed());
    b.append("totalElapsedTimeMicrosLoadShed", totalElapsedTimeMicrosLoadShed.loadRelaxed());
    b.append("totalAdmissionsLoadShed", totalAdmissionsLoadShed.loadRelaxed());
    b.append("totalQueuedTimeMicrosLoadShed", totalQueuedTimeMicrosLoadShed.loadRelaxed());
}

OperationExecutionStats::OperationExecutionStats(const OperationExecutionStats& other) {
    *this = other;
}

OperationExecutionStats& OperationExecutionStats::operator=(const OperationExecutionStats& other) {
    totalTimeQueuedMicros.storeRelaxed(other.totalTimeQueuedMicros.loadRelaxed());
    totalTimeProcessingMicros.storeRelaxed(other.totalTimeProcessingMicros.loadRelaxed());
    totalAdmissions.storeRelaxed(other.totalAdmissions.loadRelaxed());
    totalOpsFinished.storeRelaxed(other.totalOpsFinished.loadRelaxed());
    delinquencyStats = other.delinquencyStats;
    return *this;
}

OperationExecutionStats& OperationExecutionStats::operator+=(const OperationExecutionStats& other) {
    totalTimeQueuedMicros.fetchAndAddRelaxed(other.totalTimeQueuedMicros.loadRelaxed());
    totalTimeProcessingMicros.fetchAndAddRelaxed(other.totalTimeProcessingMicros.loadRelaxed());
    totalAdmissions.fetchAndAddRelaxed(other.totalAdmissions.loadRelaxed());
    totalOpsFinished.fetchAndAddRelaxed(other.totalOpsFinished.loadRelaxed());
    delinquencyStats += other.delinquencyStats;
    return *this;
}

void OperationExecutionStats::appendStats(BSONObjBuilder& b) const {
    b.append("totalTimeProcessingMicros", totalTimeProcessingMicros.loadRelaxed());
    b.append("totalTimeQueuedMicros", totalTimeQueuedMicros.loadRelaxed());
    b.append("totalAdmissions", totalAdmissions.loadRelaxed());
    b.append("totalOpsFinished", totalOpsFinished.loadRelaxed());
    delinquencyStats.appendStats(b);
}
}  // namespace mongo::admission::execution_control
