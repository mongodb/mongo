// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/query_stats_entry.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::query_stats {

// Estimated overhead for BSON document header, EOO, and potential subobject field names.
const size_t kBSONOverhead = 100;

// Estimated overhead of a single 'errors' array entry: {code, codeName, count,
// latestSeenTimestamp}.
const size_t kBSONOverheadPerErrorEntry = 100;

void QueryStatsEntry::recordErrorCode(ErrorCodes::Error code) {
    execCountErrored++;
    const auto now = Date_t::now();

    // 'find()' promotes the entry to most-recently-seen if it already exists.
    if (auto it = recentErrors.find(code); it != recentErrors.end()) {
        it->second.count++;
        it->second.latestSeenTimestamp = now;
    } else {
        recentErrors.add(code, QueryStatsErrorEntry{1, now});
    }
}

// Estimated BSON padding for a serialized QueryStatsEntry, on top of 'sizeof(QueryStatsEntry)'.
// This is an estimate of future growth, made up of:
// - overhead for the BSON structure itself, plus one allowance per top-level sub-structure
//   (cursorStats, queryExecStats, queryPlannerStats and writesStats), and
// - an additional 500 bytes to account for any potential supplemental metrics.
const size_t kQueryStatsEntryPadding = (kBSONOverhead * 5) + 500;

BSONObj QueryStatsEntry::toBSON(bool buildSubsections,
                                bool includeWriteMetrics,
                                bool includeCBRMetrics,
                                bool includeErrorMetrics) const {
    // Pad overhead for the 'errors' array based on the number of entries currently tracked.
    const size_t errorEntriesSizeEstimate =
        includeErrorMetrics ? recentErrors.size() * kBSONOverheadPerErrorEntry : 0;
    BSONObjBuilder builder{static_cast<int>(sizeof(QueryStatsEntry) + kQueryStatsEntryPadding +
                                            errorEntriesSizeEstimate)};
    builder.append("lastExecutionMicros", (long long)lastExecutionMicros);
    builder.append("execCount", (long long)execCount);
    if (includeErrorMetrics) {
        builder.append("execCountErrored", (long long)execCountErrored);
        // Serialize the bounded 'recentErrors' cache as the 'errors' array, resolving each stored
        // code to its name.
        if (!recentErrors.empty()) {
            BSONArrayBuilder errorsBuilder{builder.subarrayStart("errors")};
            for (const auto& [code, error] : recentErrors) {
                BSONObjBuilder errorBuilder{errorsBuilder.subobjStart()};
                errorBuilder.append("code", static_cast<int32_t>(code));
                errorBuilder.append("codeName", ErrorCodes::errorString(code));
                errorBuilder.append("count", (long long)error.count);
                errorBuilder.append("latestSeenTimestamp", error.latestSeenTimestamp);
            }
        }
    }
    totalExecMicros.appendTo(builder, "totalExecMicros");
    cpuNanos.appendToIfNonNegative(builder, "cpuNanos");
    workingTimeMillis.appendTo(builder, "workingTimeMillis");

    cursorStats.toBSON(builder, buildSubsections);
    queryExecStats.toBSON(builder, buildSubsections);
    queryPlannerStats.toBSON(builder, buildSubsections, includeCBRMetrics);

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
    wasMarkedNonDeprioritizable.appendTo(*builder, "wasMarkedNonDeprioritizable");

    numInterruptChecksPerSec.appendTo(*builder, "numInterruptChecksPerSec");
    overdueInterruptApproxMaxMillis.appendTo(*builder, "overdueInterruptApproxMaxMillis");

    peakTrackedMemBytes.appendTo(*builder, "peakTrackedMemBytes");
    clusterPeakTrackedMemBytes.appendTo(*builder, "clusterPeakTrackedMemBytes");

    if (buildAsSubsection) {
        queryStatsBuilder.append("queryExec", builder->obj());
    }
}

void CostBasedRankerEntry::toBSON(BSONObjBuilder& queryStatsBuilder) const {
    BSONObjBuilder cbrBuilder{sizeof(CostBasedRankerEntry) + kBSONOverhead};
    cardinalityEstimationMethods.appendTo(cbrBuilder, "cardinalityEstimationMethods");
    nDocsSampled.appendTo(cbrBuilder, "nDocsSampled");
    queryStatsBuilder.append("costBasedRanker", cbrBuilder.obj());
}

void QueryPlannerEntry::toBSON(BSONObjBuilder& queryStatsBuilder,
                               bool buildAsSubsection,
                               bool includeCBRMetrics) const {
    BSONObjBuilder* builder = &queryStatsBuilder;
    BSONObjBuilder queryPlannerBuilder{sizeof(QueryPlannerEntry) + kBSONOverhead};
    if (buildAsSubsection) {
        builder = &queryPlannerBuilder;
    }

    hasSortStage.appendTo(*builder, "hasSortStage");
    usedDisk.appendTo(*builder, "usedDisk");
    fromMultiPlanner.appendTo(*builder, "fromMultiPlanner");
    fromPlanCache.appendTo(*builder, "fromPlanCache");
    if (!planShapeCounters.empty()) {
        builder->append("planShapeCounters", planShapeCounters.toBSON());
    }

    if (includeCBRMetrics) {
        planningTimeMicros.appendTo(*builder, "planningTimeMicros");
        costBasedRankerStats.toBSON(*builder);
    }

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
    nUpdateOps.appendTo(writesBuilder, "nUpdateOps");
    nDeleteOps.appendTo(writesBuilder, "nDeleteOps");
    keysInserted.appendTo(writesBuilder, "keysInserted");
    keysDeleted.appendTo(writesBuilder, "keysDeleted");
    queryStatsBuilder.append("writes", writesBuilder.obj());
}
}  // namespace mongo::query_stats
