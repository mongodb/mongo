/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/client_cursor/cursor_response_gen.h"

#include <cstdint>

namespace mongo::query_stats {

/**
 * Represents query stats that are only (directly) available on data-bearing nodes. These metrics
 * are optionally rolled up from the data-bearing nodes to routers, and are aggregated into cursors
 * and OpDebug. This structure represents those metrics and can be used to store and aggregate them.
 */
struct DataBearingNodeMetrics {
    uint64_t keysExamined = 0;
    uint64_t docsExamined = 0;
    uint64_t bytesRead = 0;
    Microseconds readingTime{0};
    Milliseconds clusterWorkingTime{0};
    // This value will be negative if we are not running on Linux since collecting cpu time is only
    // supported on Linux systems.
    Nanoseconds cpuNanos{0};

    uint64_t delinquentAcquisitions{0};
    Milliseconds totalAcquisitionDelinquency{0};
    Milliseconds maxAcquisitionDelinquency{0};

    uint64_t numInterruptChecks{0};
    Milliseconds overdueInterruptApproxMax{0};

    bool hasSortStage : 1 = false;
    bool usedDisk : 1 = false;
    bool fromMultiPlanner : 1 = false;
    bool fromPlanCache : 1 = true;

    /**
     * Adds the fields from the given object into the fields of this object using addition (in the
     * case of numeric metrics) or conjunction/disjunction (in the case of boolean metrics).
     */
    void add(const DataBearingNodeMetrics& other) {
        keysExamined += other.keysExamined;
        docsExamined += other.docsExamined;
        bytesRead += other.bytesRead;
        readingTime += other.readingTime;
        clusterWorkingTime += other.clusterWorkingTime;
        cpuNanos += other.cpuNanos;
        delinquentAcquisitions += other.delinquentAcquisitions;
        totalAcquisitionDelinquency += other.totalAcquisitionDelinquency;
        maxAcquisitionDelinquency =
            std::max(maxAcquisitionDelinquency, other.maxAcquisitionDelinquency);
        numInterruptChecks += other.numInterruptChecks;
        overdueInterruptApproxMax =
            std::max(overdueInterruptApproxMax, other.overdueInterruptApproxMax);
        hasSortStage = hasSortStage || other.hasSortStage;
        usedDisk = usedDisk || other.usedDisk;
        fromMultiPlanner = fromMultiPlanner || other.fromMultiPlanner;
        fromPlanCache = fromPlanCache && other.fromPlanCache;
    }

    void add(const boost::optional<DataBearingNodeMetrics>& other) {
        if (other) {
            add(*other);
        }
    }

    /**
     * Aggregates the given CursorMetrics object into this one by field-wise addition (in the case
     * of numeric metrics) or disjunction (in the case of boolean metrics).
     */
    void aggregateCursorMetrics(const CursorMetrics& metrics) {
        keysExamined += metrics.getKeysExamined();
        docsExamined += metrics.getDocsExamined();
        bytesRead += metrics.getBytesRead();
        readingTime += Microseconds(metrics.getReadingTimeMicros());
        clusterWorkingTime += Milliseconds(metrics.getWorkingTimeMillis());
        cpuNanos += Nanoseconds(metrics.getCpuNanos());
        delinquentAcquisitions += metrics.getDelinquentAcquisitions();
        totalAcquisitionDelinquency += Milliseconds(metrics.getTotalAcquisitionDelinquencyMillis());
        maxAcquisitionDelinquency = Milliseconds(std::max(
            maxAcquisitionDelinquency.count(), metrics.getMaxAcquisitionDelinquencyMillis()));
        numInterruptChecks += metrics.getNumInterruptChecks();
        overdueInterruptApproxMax = Milliseconds{std::max(
            overdueInterruptApproxMax.count(), metrics.getOverdueInterruptApproxMaxMillis())};
        hasSortStage = hasSortStage || metrics.getHasSortStage();
        usedDisk = usedDisk || metrics.getUsedDisk();
        fromMultiPlanner = fromMultiPlanner || metrics.getFromMultiPlanner();
        fromPlanCache = fromPlanCache && metrics.getFromPlanCache();
    }
};

}  // namespace mongo::query_stats
