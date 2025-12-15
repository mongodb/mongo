/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/query_stats_entry.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::query_stats {

// Estimated overhead for BSON document header, EOO, and potential subobject field names.
const size_t kBSONOverhead = 100;

BSONObj QueryStatsEntry::toBSON(bool buildSubsections, bool includeWriteMetrics) const {
    // Pad not only for the overhead of the BSON structure itself, but also for the
    // sub-structures it contains. We have 4 sub-structures at the top level:
    // cursorStats, queryExecStats, queryPlannerStats, and writesStats.
    // Pad an additional 500 bytes to account for any potential supplemental metrics.
    BSONObjBuilder builder{sizeof(QueryStatsEntry) + (kBSONOverhead * 5) + 500};
    builder.append("lastExecutionMicros", (long long)lastExecutionMicros);
    builder.append("execCount", (long long)execCount);
    totalExecMicros.appendTo(builder, "totalExecMicros");
    cpuNanos.appendToIfNonNegative(builder, "cpuNanos");
    workingTimeMillis.appendTo(builder, "workingTimeMillis");

    cursorStats.toBSON(builder, buildSubsections);
    queryExecStats.toBSON(builder, buildSubsections);
    queryPlannerStats.toBSON(builder, buildSubsections);

    if (includeWriteMetrics) {
        writesStats.toBSON(builder);
    }

    builder.append("firstSeenTimestamp", firstSeenTimestamp);
    builder.append("latestSeenTimestamp", latestSeenTimestamp);
    if (supplementalStatsMap) {
        builder.append("supplementalMetrics", supplementalStatsMap->toBSON());
    }
    return builder.obj();
}

void QueryStatsEntry::addSupplementalStats(std::unique_ptr<SupplementalStatsEntry> metric) {
    if (metric) {
        if (!supplementalStatsMap) {
            supplementalStatsMap = std::make_unique<SupplementalStatsMap>();
        }
        supplementalStatsMap->update(std::move(metric));
    }
}

void QueryExecEntry::toBSON(BSONObjBuilder& queryStatsBuilder, bool buildAsSubsection) const {
    BSONObjBuilder* builder = &queryStatsBuilder;
    BSONObjBuilder queryExecBuilder{sizeof(QueryExecEntry) + kBSONOverhead};
    if (buildAsSubsection) {
        builder = &queryExecBuilder;
    }

    docsReturned.appendTo(*builder, "docsReturned");

    // Disk use metrics.
    keysExamined.appendTo(*builder, "keysExamined");
    docsExamined.appendTo(*builder, "docsExamined");
    bytesRead.appendTo(*builder, "bytesRead");
    readTimeMicros.appendTo(*builder, "readTimeMicros");

    delinquentAcquisitions.appendTo(*builder, "delinquentAcquisitions");
    totalAcquisitionDelinquencyMillis.appendTo(*builder, "totalAcquisitionDelinquencyMillis");
    maxAcquisitionDelinquencyMillis.appendTo(*builder, "maxAcquisitionDelinquencyMillis");

    totalTimeQueuedMicros.appendTo(*builder, "totalTimeQueuedMicros");
    totalAdmissions.appendTo(*builder, "totalAdmissions");
    wasLoadShed.appendTo(*builder, "wasLoadShed");
    wasDeprioritized.appendTo(*builder, "wasDeprioritized");

    numInterruptChecksPerSec.appendTo(*builder, "numInterruptChecksPerSec");
    overdueInterruptApproxMaxMillis.appendTo(*builder, "overdueInterruptApproxMaxMillis");

    if (buildAsSubsection) {
        queryStatsBuilder.append("queryExec", builder->obj());
    }
}

void QueryPlannerEntry::toBSON(BSONObjBuilder& queryStatsBuilder, bool buildAsSubsection) const {
    BSONObjBuilder* builder = &queryStatsBuilder;
    BSONObjBuilder queryPlannerBuilder{sizeof(QueryPlannerEntry) + kBSONOverhead};
    if (buildAsSubsection) {
        builder = &queryPlannerBuilder;
    }

    hasSortStage.appendTo(*builder, "hasSortStage");
    usedDisk.appendTo(*builder, "usedDisk");
    fromMultiPlanner.appendTo(*builder, "fromMultiPlanner");
    fromPlanCache.appendTo(*builder, "fromPlanCache");

    if (buildAsSubsection) {
        queryStatsBuilder.append("queryPlanner", builder->obj());
    }
}

void CursorEntry::toBSON(BSONObjBuilder& queryStatsBuilder, bool buildAsSubsection) const {
    BSONObjBuilder* builder = &queryStatsBuilder;
    BSONObjBuilder cursorBuilder{sizeof(CursorEntry) + kBSONOverhead};
    if (buildAsSubsection) {
        builder = &cursorBuilder;
    }

    firstResponseExecMicros.appendTo(*builder, "firstResponseExecMicros");

    if (buildAsSubsection) {
        queryStatsBuilder.append("cursor", builder->obj());
    }
}

void WritesEntry::toBSON(BSONObjBuilder& queryStatsBuilder) const {
    BSONObjBuilder writesBuilder{sizeof(WritesEntry) + kBSONOverhead};
    nMatched.appendTo(writesBuilder, "nMatched");
    nUpserted.appendTo(writesBuilder, "nUpserted");
    nModified.appendTo(writesBuilder, "nModified");
    nDeleted.appendTo(writesBuilder, "nDeleted");
    nInserted.appendTo(writesBuilder, "nInserted");
    queryStatsBuilder.append("writes", writesBuilder.obj());
}
}  // namespace mongo::query_stats
