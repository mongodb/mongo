// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/execution_control/execution_control_stats.h"

#include "mongo/logv2/log.h"

#include <algorithm>
#include <iterator>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::admission::execution_control {

void QueueWaitTimeHistogram::record(Microseconds queueWaitTime) {
    _hist.increment(std::max<int64_t>(0, queueWaitTime.count()));
}

void QueueWaitTimeHistogram::appendStats(BSONArrayBuilder& arr) const {
    for (auto&& bucket : _hist) {
        // The lowermost bucket has no finite lower bound; wait time is non-negative so report 0.
        BSONObjBuilder bob(arr.subobjStart());
        bob.append("lowerBound", bucket.lower ? *bucket.lower : int64_t{0});
        bob.append("count", bucket.count);
    }
}

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
    totalOpsFinished.storeRelaxed(other.totalOpsFinished.loadRelaxed());
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
    totalOpsFinished.fetchAndAddRelaxed(other.totalOpsFinished.loadRelaxed());
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
    b.append("totalOpsFinished", totalOpsFinished.loadRelaxed());
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
    totalNormalPriorityAdmissions.storeRelaxed(other.totalNormalPriorityAdmissions.loadRelaxed());
    totalLowPriorityAdmissions.storeRelaxed(other.totalLowPriorityAdmissions.loadRelaxed());
    delinquencyStats = other.delinquencyStats;
    return *this;
}

OperationExecutionStats& OperationExecutionStats::operator+=(const OperationExecutionStats& other) {
    totalTimeQueuedMicros.fetchAndAddRelaxed(other.totalTimeQueuedMicros.loadRelaxed());
    totalTimeProcessingMicros.fetchAndAddRelaxed(other.totalTimeProcessingMicros.loadRelaxed());
    totalAdmissions.fetchAndAddRelaxed(other.totalAdmissions.loadRelaxed());
    totalNormalPriorityAdmissions.fetchAndAddRelaxed(
        other.totalNormalPriorityAdmissions.loadRelaxed());
    totalLowPriorityAdmissions.fetchAndAddRelaxed(other.totalLowPriorityAdmissions.loadRelaxed());
    delinquencyStats += other.delinquencyStats;
    return *this;
}

void OperationExecutionStats::appendStats(BSONObjBuilder& b) const {
    b.append("totalTimeProcessingMicros", totalTimeProcessingMicros.loadRelaxed());
    b.append("totalTimeQueuedMicros", totalTimeQueuedMicros.loadRelaxed());
    b.append("totalAdmissions", totalAdmissions.loadRelaxed());
    b.append("totalNormalPriorityAdmissions", totalNormalPriorityAdmissions.loadRelaxed());
    b.append("totalLowPriorityAdmissions", totalLowPriorityAdmissions.loadRelaxed());
    delinquencyStats.appendStats(b);
}
}  // namespace mongo::admission::execution_control
