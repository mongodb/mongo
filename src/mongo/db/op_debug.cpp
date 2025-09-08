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

#include "mongo/db/op_debug.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_bson_helpers.h"
#include "mongo/db/local_catalog/local_oplog_info.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/ticketholder_queue_stats.h"
#include "mongo/util/duration.h"

#include <string>

namespace mongo {
namespace {
StringData getProtoString(int op) {
    if (op == dbMsg) {
        return "op_msg";
    } else if (op == dbQuery) {
        return "op_query";
    }
    MONGO_UNREACHABLE;
}

template <typename AppendCallback>
void addSingleSpillingStats(PlanSummaryStats::SpillingStage stage,
                            const SpillingStats& stats,
                            size_t sortTotalDataSizeBytes,
                            const AppendCallback& appendCallback) {
    // Attributes for logs don't support dynamically generated strings as attribute names, so we
    // have to hard-code all attribute names.
    switch (stage) {
        case PlanSummaryStats::SpillingStage::BUCKET_AUTO:
            appendCallback("bucketAutoSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("bucketAutoSpilledBytes",
                           static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("bucketAutoSpilledRecords",
                           static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("bucketAutoSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            return;
        case PlanSummaryStats::SpillingStage::GEO_NEAR:
            appendCallback("geoNearSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("geoNearSpilledBytes", static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("geoNearSpilledRecords",
                           static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("geoNearSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            return;
        case PlanSummaryStats::SpillingStage::GRAPH_LOOKUP:
            appendCallback("graphLookupSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("graphLookupSpilledBytes",
                           static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("graphLookupSpilledRecords",
                           static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("graphLookupSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            return;
        case PlanSummaryStats::SpillingStage::GROUP:
            appendCallback("groupSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("groupSpilledBytes", static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("groupSpilledRecords",
                           static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("groupSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            return;
        case PlanSummaryStats::SpillingStage::SET_WINDOW_FIELDS:
            appendCallback("setWindowFieldsSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("setWindowFieldsSpilledBytes",
                           static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("setWindowFieldsSpilledRecords",
                           static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("setWindowFieldsSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            return;
        case PlanSummaryStats::SpillingStage::SORT:
            appendCallback("sortSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("sortSpilledBytes", static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("sortSpilledRecords", static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("sortSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            // Extra sort-specific metric
            appendCallback("sortTotalDataSizeBytes",
                           static_cast<long long>(sortTotalDataSizeBytes));
            return;
        case PlanSummaryStats::SpillingStage::TEXT_OR:
            appendCallback("textOrSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("textOrSpilledBytes", static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("textOrSpilledRecords",
                           static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("textOrSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            return;
        case PlanSummaryStats::SpillingStage::HASH_LOOKUP:
            appendCallback("hashLookupSpills", static_cast<long long>(stats.getSpills()));
            appendCallback("hashLookupSpilledBytes",
                           static_cast<long long>(stats.getSpilledBytes()));
            appendCallback("hashLookupSpilledRecords",
                           static_cast<long long>(stats.getSpilledRecords()));
            appendCallback("hashLookupSpilledDataStorageSize",
                           static_cast<long long>(stats.getSpilledDataStorageSize()));
            return;
    }
    MONGO_UNREACHABLE_TASSERT(9851000);
}

template <typename AppendCallback>
void addSpillingStats(const absl::flat_hash_map<PlanSummaryStats::SpillingStage, SpillingStats>&
                          spillingStatsPerStage,
                      size_t sortTotalDataSizeBytes,
                      const AppendCallback& appendCallback) {
    for (const auto& [stage, stats] : spillingStatsPerStage) {
        addSingleSpillingStats(stage, stats, sortTotalDataSizeBytes, appendCallback);
    }
}
}  // namespace

#define OPDEBUG_TOSTRING_HELP(x) \
    if (x >= 0)                  \
    s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_BOOL(x) \
    if (x)                            \
    s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_ATOMIC(x, y) \
    if (auto __y = y.load(); __y > 0)      \
    s << " " x ":" << (__y)
#define OPDEBUG_TOSTRING_HELP_OPTIONAL(x, y) \
    if (y)                                   \
    s << " " x ":" << (*y)

#define OPDEBUG_TOATTR_HELP(x) \
    if (x >= 0)                \
    pAttrs->add(#x, x)
#define OPDEBUG_TOATTR_HELP_BOOL_NAMED(name, x) \
    if (x)                                      \
    pAttrs->add(name, x)
#define OPDEBUG_TOATTR_HELP_BOOL(x) OPDEBUG_TOATTR_HELP_BOOL_NAMED(#x, x)
#define OPDEBUG_TOATTR_HELP_ATOMIC(x, y) \
    if (auto __y = y.load(); __y > 0)    \
    pAttrs->add(x, __y)
#define OPDEBUG_TOATTR_HELP_OPTIONAL(x, y) \
    if (y)                                 \
    pAttrs->add(x, *y)

void OpDebug::report(OperationContext* opCtx,
                     const SingleThreadedLockStats* lockStats,
                     const SingleThreadedStorageMetrics& storageMetrics,
                     long long prepareReadConflicts,
                     logv2::DynamicAttributes* pAttrs) const {
    Client* client = opCtx->getClient();
    auto& curop = *CurOp::get(opCtx);
    auto flowControlStats = shard_role_details::getLocker(opCtx)->getFlowControlStats();

    if (iscommand) {
        pAttrs->add("type", "command");
    } else {
        pAttrs->add("type", networkOpToString(networkOp));
    }

    pAttrs->add("isFromUserConnection", client && client->isFromUserConnection());
    pAttrs->addDeepCopy("ns", toStringForLogging(curop.getNSS()));
    pAttrs->addDeepCopy("collectionType", getCollectionType(curop.getNSS()));

    if (client) {
        if (auto clientMetadata = ClientMetadata::get(client)) {
            StringData appName = clientMetadata->getApplicationName();
            if (!appName.empty()) {
                pAttrs->add("appName", appName);
            }
        }
    }

    auto query = curop_bson_helpers::appendCommentField(opCtx, curop.opDescription());
    if (!query.isEmpty()) {
        if (auto&& queryShapeHash = getQueryShapeHash()) {
            pAttrs->addDeepCopy("queryShapeHash", queryShapeHash->toHexString());
        }
        if (iscommand) {
            const Command* curCommand = curop.getCommand();
            if (curCommand) {
                mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
                curCommand->snipForLogging(&cmdToLog);
                pAttrs->add("command", redact(cmdToLog.getObject()));
            } else {
                // Should not happen but we need to handle curCommand == NULL gracefully.
                // We don't know what the request payload is intended to be, so it might be
                // sensitive, and we don't know how to redact it properly without a 'Command*'.
                // So we just don't log it at all.
                pAttrs->add("command", "unrecognized");
            }
        } else {
            pAttrs->add("command", redact(query));
        }
    }

    auto originatingCommand = curop.originatingCommand();
    if (!originatingCommand.isEmpty()) {
        pAttrs->add("originatingCommand", redact(originatingCommand));
    }

    if (!curop.getPlanSummary().empty()) {
        pAttrs->addDeepCopy("planSummary", std::string{curop.getPlanSummary()});
    }

    if (planningTime > Microseconds::zero()) {
        pAttrs->add("planningTimeMicros", durationCount<Microseconds>(planningTime));
    }

    if (estimatedCost) {
        pAttrs->add("estimatedCost", *estimatedCost);
    }

    if (estimatedCardinality) {
        pAttrs->add("estimatedCardinality", *estimatedCardinality);
    }

    if (prepareConflictDurationMillis > Milliseconds::zero()) {
        pAttrs->add("prepareConflictDuration", prepareConflictDurationMillis);
    }

    if (catalogCacheDatabaseLookupMillis > Milliseconds::zero()) {
        pAttrs->add("catalogCacheDatabaseLookupDuration", catalogCacheDatabaseLookupMillis);
    }

    if (catalogCacheCollectionLookupMillis > Milliseconds::zero()) {
        pAttrs->add("catalogCacheCollectionLookupDuration", catalogCacheCollectionLookupMillis);
    }

    if (catalogCacheIndexLookupMillis > Milliseconds::zero()) {
        pAttrs->add("catalogCacheIndexLookupDuration", catalogCacheIndexLookupMillis);
    }

    if (databaseVersionRefreshMillis > Milliseconds::zero()) {
        pAttrs->add("databaseVersionRefreshDuration", databaseVersionRefreshMillis);
    }

    if (placementVersionRefreshMillis > Milliseconds::zero()) {
        pAttrs->add("placementVersionRefreshDuration", placementVersionRefreshMillis);
    }

    if (auto totalOplogSlotDurationMicros =
            LocalOplogInfo::getOplogSlotTimeContext(opCtx).getTotalMicros();
        totalOplogSlotDurationMicros > Microseconds::zero()) {
        pAttrs->add("totalOplogSlotDuration", totalOplogSlotDurationMicros);
    }

    if (dataThroughputLastSecond) {
        pAttrs->add("dataThroughputLastSecondMBperSec", *dataThroughputLastSecond);
    }

    if (dataThroughputAverage) {
        pAttrs->add("dataThroughputAverageMBPerSec", *dataThroughputAverage);
    }

    if (!resolvedViews.empty()) {
        pAttrs->add("resolvedViews", getResolvedViewsInfo());
    }

    OPDEBUG_TOATTR_HELP(nShards);
    OPDEBUG_TOATTR_HELP(cursorid);
    if (mongotCursorId) {
        pAttrs->add("mongot", makeMongotDebugStatsObject());
    }
    OPDEBUG_TOATTR_HELP_BOOL(exhaust);

    OPDEBUG_TOATTR_HELP_OPTIONAL("keysExamined", additiveMetrics.keysExamined);
    OPDEBUG_TOATTR_HELP_OPTIONAL("docsExamined", additiveMetrics.docsExamined);

    OPDEBUG_TOATTR_HELP_BOOL_NAMED("hasSortStage", additiveMetrics.hasSortStage);
    OPDEBUG_TOATTR_HELP_BOOL_NAMED("usedDisk", additiveMetrics.usedDisk);
    OPDEBUG_TOATTR_HELP_BOOL_NAMED("fromMultiPlanner", additiveMetrics.fromMultiPlanner);
    OPDEBUG_TOATTR_HELP_BOOL_NAMED("fromPlanCache", additiveMetrics.fromPlanCache.value_or(false));
    if (replanReason) {
        bool replanned = true;
        OPDEBUG_TOATTR_HELP_BOOL(replanned);
        pAttrs->add("replanReason", redact(*replanReason));
    }
    OPDEBUG_TOATTR_HELP_OPTIONAL("nMatched", additiveMetrics.nMatched);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nBatches", additiveMetrics.nBatches);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nModified", additiveMetrics.nModified);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ninserted", additiveMetrics.ninserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ndeleted", additiveMetrics.ndeleted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nUpserted", additiveMetrics.nUpserted);
    OPDEBUG_TOATTR_HELP_BOOL(cursorExhausted);

    OPDEBUG_TOATTR_HELP_OPTIONAL("keysInserted", additiveMetrics.keysInserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysDeleted", additiveMetrics.keysDeleted);

    if (prepareReadConflicts > 0) {
        pAttrs->add("prepareReadConflicts", prepareReadConflicts);
    }

    if (storageMetrics.writeConflicts > 0) {
        pAttrs->add("writeConflicts", storageMetrics.writeConflicts);
    }

    if (storageMetrics.interruptResponseNs > 0) {
        pAttrs->add("storageInterruptResponseNanos", storageMetrics.interruptResponseNs);
    }

    if (storageMetrics.temporarilyUnavailableErrors > 0) {
        pAttrs->add("temporarilyUnavailableErrors", storageMetrics.temporarilyUnavailableErrors);
    }

    pAttrs->add("numYields", curop.numYields());
    OPDEBUG_TOATTR_HELP_OPTIONAL("nreturned", additiveMetrics.nreturned);

    addSpillingStats(spillingStatsPerStage,
                     sortTotalDataSizeBytes,
                     [&](const auto& name, const auto& value) { pAttrs->add(name, value); });

    if (int64_t inUseTrackedMemBytes = curop.getInUseTrackedMemoryBytes()) {
        pAttrs->add("inUseTrackedMemBytes", inUseTrackedMemBytes);
    }
    if (int64_t peakTrackedMemBytes = curop.getPeakTrackedMemoryBytes()) {
        pAttrs->add("peakTrackedMemBytes", peakTrackedMemBytes);
    }
    if (planCacheShapeHash) {
        // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
        std::string planCacheShapeHashStr = zeroPaddedHex(*planCacheShapeHash);
        pAttrs->addDeepCopy("planCacheShapeHash", planCacheShapeHashStr);
        pAttrs->addDeepCopy("queryHash", planCacheShapeHashStr);
    }
    if (planCacheKey) {
        pAttrs->addDeepCopy("planCacheKey", zeroPaddedHex(*planCacheKey));
    }

    switch (queryFramework) {
        case PlanExecutor::QueryFramework::kClassicOnly:
        case PlanExecutor::QueryFramework::kClassicHybrid:
            pAttrs->add("queryFramework", "classic");
            break;
        case PlanExecutor::QueryFramework::kSBEOnly:
        case PlanExecutor::QueryFramework::kSBEHybrid:
            pAttrs->add("queryFramework", "sbe");
            break;
        case PlanExecutor::QueryFramework::kUnknown:
            break;
    }

    if (!errInfo.isOK()) {
        pAttrs->add("ok", 0);
        if (!errInfo.reason().empty()) {
            pAttrs->add("errMsg", redact(errInfo.reason()));
        }
        pAttrs->addDeepCopy("errName", errInfo.codeString());
        pAttrs->add("errCode", static_cast<int>(errInfo.code()));
    }

    if (responseLength > 0) {
        pAttrs->add("reslen", responseLength);
    }

    if (lockStats) {
        BSONObjBuilder locks;
        lockStats->report(&locks);
        pAttrs->add("locks", locks.obj());
    }

    auto userAcquisitionStats = curop.getUserAcquisitionStats();
    if (userAcquisitionStats->shouldReportUserCacheAccessStats()) {
        BSONObjBuilder userCacheAcquisitionStatsBuilder;
        userAcquisitionStats->reportUserCacheAcquisitionStats(
            &userCacheAcquisitionStatsBuilder, opCtx->getServiceContext()->getTickSource());
        pAttrs->add("authorization", userCacheAcquisitionStatsBuilder.obj());
    }

    if (userAcquisitionStats->shouldReportLDAPOperationStats()) {
        BSONObjBuilder ldapOperationStatsBuilder;
        userAcquisitionStats->reportLdapOperationStats(&ldapOperationStatsBuilder,
                                                       opCtx->getServiceContext()->getTickSource());
        pAttrs->add("LDAPOperations", ldapOperationStatsBuilder.obj());
    }

    BSONObj flowControlObj = makeFlowControlObject(flowControlStats);
    if (flowControlObj.nFields() > 0) {
        pAttrs->add("flowControl", flowControlObj);
    }

    {
        const auto& readConcern = repl::ReadConcernArgs::get(opCtx);
        if (readConcern.isSpecified()) {
            pAttrs->add("readConcern", readConcern.toBSONInner());
        }
    }

    if (writeConcern && !writeConcern->usedDefaultConstructedWC) {
        pAttrs->add("writeConcern", writeConcern->toBSON());
    }

    if (waitForWriteConcernDurationMillis > Milliseconds::zero()) {
        pAttrs->add("waitForWriteConcernDuration", waitForWriteConcernDurationMillis);
    }

    if (storageStats) {
        pAttrs->add("storage", storageStats->toBSON());
    }

    if (spillStorageStats) {
        pAttrs->add("spillStorage", spillStorageStats->toBSON());
    }

    // Always report cpuNanos in rare cases that it is zero to facilitate testing that expects this
    // field to always exist.
    if (cpuTime >= Nanoseconds::zero()) {
        pAttrs->add("cpuNanos", durationCount<Nanoseconds>(cpuTime));
    }

    if (client && client->session()) {
        pAttrs->add("remote", client->session()->remote());
    }

    if (iscommand) {
        pAttrs->add("protocol", getProtoString(networkOp));
    }

    if (const auto& invocation = CommandInvocation::get(opCtx);
        invocation && invocation->isMirrored()) {
        const bool mirrored = true;
        OPDEBUG_TOATTR_HELP_BOOL(mirrored);
    }

    if (remoteOpWaitTime) {
        pAttrs->add("remoteOpWaitMillis", durationCount<Milliseconds>(*remoteOpWaitTime));
    }

    if (!curop.parent()) {
        pAttrs->add("numInterruptChecks", opCtx->numInterruptChecks());

        const auto& admCtx = ExecutionAdmissionContext::get(opCtx);
        // Note that we don't record delinquency stats around ticketing when in a
        // multi-document transaction, since operations within multi-document transactions hold
        // tickets for a long time by design and reporting them as delinquent will just create
        // noise in the data.
        const bool reportAcquisitions = !opCtx->inMultiDocumentTransaction();
        const auto* stats = opCtx->overdueInterruptCheckStats();
        if ((reportAcquisitions && admCtx.getDelinquentAcquisitions() > 0) ||
            (stats && stats->overdueInterruptChecks.loadRelaxed() > 0)) {
            BSONObjBuilder sub;
            appendDelinquentInfo(opCtx, sub, reportAcquisitions);
            pAttrs->add("delinquencyInfo", sub.obj());
        }
    }

    // Extract admission and execution control queueing stats from AdmissionContext stored on opCtx
    TicketHolderQueueStats queueingStats(opCtx);
    pAttrs->add("queues", queueingStats.toBson());

    // workingMillis should always be present for any operation
    // TODO (SERVER-103038) add `workingMillis` field only if it's value has been set.
    pAttrs->add("workingMillis", workingTimeMillis.count());

    // Measures the time from when the operation was killed to when it completed.
    if (auto killTime = opCtx->getKillTime()) {
        auto ts = opCtx->getServiceContext()->getTickSource();
        pAttrs->add(
            "interruptLatencyNanos",
            durationCount<Nanoseconds>(ts->ticksTo<Nanoseconds>(ts->getTicks() - killTime)));
    }

    // durationMillis should always be present for any operation
    pAttrs->add("durationMillis",
                durationCount<Milliseconds>(CurOp::get(opCtx)->elapsedTimeTotal()));

    // ~~~~~~~~~~ NOTHING BELOW HERE ~~~~~~~~~~
    // We want durationMillis to be the last field to make it easier to find visually in slow query
    // logs.
}

void OpDebug::reportStorageStats(logv2::DynamicAttributes* pAttrs) const {
    if (storageStats) {
        pAttrs->add("storage", storageStats->toBSON());
    }
}

#define OPDEBUG_APPEND_NUMBER2(b, x, y) \
    if (y != -1)                        \
    (b).appendNumber(x, (y))
#define OPDEBUG_APPEND_NUMBER(b, x) OPDEBUG_APPEND_NUMBER2(b, #x, x)

#define OPDEBUG_APPEND_BOOL2(b, x, y) \
    if (y)                            \
    (b).appendBool(x, (y))
#define OPDEBUG_APPEND_BOOL(b, x) OPDEBUG_APPEND_BOOL2(b, #x, x)

#define OPDEBUG_APPEND_ATOMIC(b, x, y) \
    if (auto __y = y.load(); __y > 0)  \
    (b).appendNumber(x, __y)
#define OPDEBUG_APPEND_OPTIONAL(b, x, y) \
    if (y)                               \
    (b).appendNumber(x, (*y))

static constexpr size_t appendMaxElementSize = 50ull * 1024;

void OpDebug::append(OperationContext* opCtx,
                     const SingleThreadedLockStats& lockStats,
                     FlowControlTicketholder::CurOp flowControlStats,
                     const SingleThreadedStorageMetrics& storageMetrics,
                     long long prepareReadConflicts,
                     bool omitCommand,
                     BSONObjBuilder& b) const {
    auto& curop = *CurOp::get(opCtx);

    b.append("op", logicalOpToString(logicalOp));

    b.append("ns", curop.getNS());

    if (!omitCommand) {
        curop_bson_helpers::appendObjectTruncatingAsNecessary(
            "command",
            curop_bson_helpers::appendCommentField(opCtx, curop.opDescription()),
            appendMaxElementSize,
            b);

        auto originatingCommand = curop.originatingCommand();
        if (!originatingCommand.isEmpty()) {
            curop_bson_helpers::appendObjectTruncatingAsNecessary(
                "originatingCommand", originatingCommand, appendMaxElementSize, b);
        }
    }

    if (!resolvedViews.empty()) {
        appendResolvedViewsInfo(b);
    }

    OPDEBUG_APPEND_NUMBER(b, nShards);
    OPDEBUG_APPEND_NUMBER(b, cursorid);
    if (mongotCursorId) {
        b.append("mongot", makeMongotDebugStatsObject());
    }
    OPDEBUG_APPEND_BOOL(b, exhaust);

    OPDEBUG_APPEND_OPTIONAL(b, "keysExamined", additiveMetrics.keysExamined);
    OPDEBUG_APPEND_OPTIONAL(b, "docsExamined", additiveMetrics.docsExamined);

    if (int64_t inUseTrackedMemBytes = curop.getInUseTrackedMemoryBytes()) {
        b.append("inUseTrackedMemBytes", inUseTrackedMemBytes);
    }
    if (int64_t peakTrackedMemBytes = curop.getPeakTrackedMemoryBytes()) {
        b.append("peakTrackedMemBytes", peakTrackedMemBytes);
    }

    OPDEBUG_APPEND_BOOL2(b, "hasSortStage", additiveMetrics.hasSortStage);
    OPDEBUG_APPEND_BOOL2(b, "usedDisk", additiveMetrics.usedDisk);
    OPDEBUG_APPEND_BOOL2(b, "fromMultiPlanner", additiveMetrics.fromMultiPlanner);
    OPDEBUG_APPEND_BOOL2(b, "fromPlanCache", additiveMetrics.fromPlanCache.value_or(false));
    if (replanReason) {
        bool replanned = true;
        OPDEBUG_APPEND_BOOL(b, replanned);
        b.append("replanReason", *replanReason);
    }
    OPDEBUG_APPEND_OPTIONAL(b, "nMatched", additiveMetrics.nMatched);
    OPDEBUG_APPEND_OPTIONAL(b, "nBatches", additiveMetrics.nBatches);
    OPDEBUG_APPEND_OPTIONAL(b, "nModified", additiveMetrics.nModified);
    OPDEBUG_APPEND_OPTIONAL(b, "ninserted", additiveMetrics.ninserted);
    OPDEBUG_APPEND_OPTIONAL(b, "ndeleted", additiveMetrics.ndeleted);
    OPDEBUG_APPEND_OPTIONAL(b, "nUpserted", additiveMetrics.nUpserted);
    OPDEBUG_APPEND_BOOL(b, cursorExhausted);

    OPDEBUG_APPEND_OPTIONAL(b, "keysInserted", additiveMetrics.keysInserted);
    OPDEBUG_APPEND_OPTIONAL(b, "keysDeleted", additiveMetrics.keysDeleted);
    if (prepareReadConflicts > 0) {
        b.append("prepareReadConflicts", prepareReadConflicts);
    }
    if (storageMetrics.writeConflicts > 0) {
        b.append("writeConflicts", storageMetrics.writeConflicts);
    }
    if (storageMetrics.interruptResponseNs > 0) {
        b.append("storageInterruptResponseNanos", storageMetrics.interruptResponseNs);
    }
    if (storageMetrics.temporarilyUnavailableErrors > 0) {
        b.append("temporarilyUnavailableErrors", storageMetrics.temporarilyUnavailableErrors);
    }

    OPDEBUG_APPEND_OPTIONAL(b, "dataThroughputLastSecond", dataThroughputLastSecond);
    OPDEBUG_APPEND_OPTIONAL(b, "dataThroughputAverage", dataThroughputAverage);

    b.appendNumber("numYield", curop.numYields());
    OPDEBUG_APPEND_OPTIONAL(b, "nreturned", additiveMetrics.nreturned);

    addSpillingStats(spillingStatsPerStage,
                     sortTotalDataSizeBytes,
                     [&](const auto& name, const auto& value) { b.append(name, value); });

    if (planCacheShapeHash) {
        // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
        std::string planCacheShapeHashStr = zeroPaddedHex(*planCacheShapeHash);
        b.append("planCacheShapeHash", planCacheShapeHashStr);
        b.append("queryHash", planCacheShapeHashStr);
    }
    if (planCacheKey) {
        b.append("planCacheKey", zeroPaddedHex(*planCacheKey));
    }
    if (auto&& queryShapeHash = getQueryShapeHash()) {
        b.append("queryShapeHash", queryShapeHash->toHexString());
    }

    switch (queryFramework) {
        case PlanExecutor::QueryFramework::kClassicOnly:
        case PlanExecutor::QueryFramework::kClassicHybrid:
            b.append("queryFramework", "classic");
            break;
        case PlanExecutor::QueryFramework::kSBEOnly:
        case PlanExecutor::QueryFramework::kSBEHybrid:
            b.append("queryFramework", "sbe");
            break;
        case PlanExecutor::QueryFramework::kUnknown:
            break;
    }

    {
        BSONObjBuilder locks(b.subobjStart("locks"));
        lockStats.report(&locks);
    }

    {
        auto userAcquisitionStats = curop.getUserAcquisitionStats();
        if (userAcquisitionStats->shouldReportUserCacheAccessStats()) {
            BSONObjBuilder userCacheAcquisitionStatsBuilder(b.subobjStart("authorization"));
            userAcquisitionStats->reportUserCacheAcquisitionStats(
                &userCacheAcquisitionStatsBuilder, opCtx->getServiceContext()->getTickSource());
        }

        if (userAcquisitionStats->shouldReportLDAPOperationStats()) {
            BSONObjBuilder ldapOperationStatsBuilder;
            userAcquisitionStats->reportLdapOperationStats(
                &ldapOperationStatsBuilder, opCtx->getServiceContext()->getTickSource());
        }
    }

    {
        BSONObj flowControlMetrics = makeFlowControlObject(flowControlStats);
        BSONObjBuilder flowControlBuilder(b.subobjStart("flowControl"));
        flowControlBuilder.appendElements(flowControlMetrics);
    }

    {
        const auto& readConcern = repl::ReadConcernArgs::get(opCtx);
        if (readConcern.isSpecified()) {
            readConcern.appendInfo(&b);
        }
    }

    if (writeConcern && !writeConcern->usedDefaultConstructedWC) {
        b.append("writeConcern", writeConcern->toBSON());
    }

    if (waitForWriteConcernDurationMillis > Milliseconds::zero()) {
        b.append("waitForWriteConcernDuration",
                 durationCount<Milliseconds>(waitForWriteConcernDurationMillis));
    }

    if (storageStats) {
        b.append("storage", storageStats->toBSON());
    }

    if (spillStorageStats) {
        b.append("spillStorage", spillStorageStats->toBSON());
    }

    if (!errInfo.isOK()) {
        b.appendNumber("ok", 0.0);
        if (!errInfo.reason().empty()) {
            b.append("errMsg", errInfo.reason());
        }
        b.append("errName", ErrorCodes::errorString(errInfo.code()));
        b.append("errCode", errInfo.code());
    }

    OPDEBUG_APPEND_NUMBER(b, responseLength);
    if (iscommand) {
        b.append("protocol", getProtoString(networkOp));
    }

    if (remoteOpWaitTime) {
        b.append("remoteOpWaitMillis", durationCount<Milliseconds>(*remoteOpWaitTime));
    }

    // Always log cpuNanos in rare cases that it is zero to facilitate testing that expects this
    // field to always exist.
    if (cpuTime >= Nanoseconds::zero()) {
        b.appendNumber("cpuNanos", durationCount<Nanoseconds>(cpuTime));
    }

    // millis should always be present for any operation
    b.appendNumber(
        "millis",
        durationCount<Milliseconds>(additiveMetrics.executionTime.value_or(Microseconds{0})));

    if (!curop.getPlanSummary().empty()) {
        b.append("planSummary", curop.getPlanSummary());
    }

    if (planningTime > Microseconds::zero()) {
        b.appendNumber("planningTimeMicros", durationCount<Microseconds>(planningTime));
    }

    OPDEBUG_APPEND_OPTIONAL(b, "estimatedCost", estimatedCost);

    OPDEBUG_APPEND_OPTIONAL(b, "estimatedCardinality", estimatedCardinality);

    if (auto totalOplogSlotDurationMicros =
            LocalOplogInfo::getOplogSlotTimeContext(opCtx).getTotalMicros();
        totalOplogSlotDurationMicros > Microseconds::zero()) {
        b.appendNumber("totalOplogSlotDurationMicros",
                       durationCount<Microseconds>(totalOplogSlotDurationMicros));
    }

    if (!execStats.isEmpty()) {
        b.append("execStats", std::move(execStats));
    }

    // Measures the time from when the operation was killed to when it completed.
    if (auto killTime = opCtx->getKillTime()) {
        auto ts = opCtx->getServiceContext()->getTickSource();
        b.appendNumber(
            "interruptLatencyNanos",
            durationCount<Nanoseconds>(ts->ticksTo<Nanoseconds>(ts->getTicks() - killTime)));
    }
}

void OpDebug::appendUserInfo(const CurOp& c,
                             BSONObjBuilder& builder,
                             AuthorizationSession* authSession) {
    std::string opdb(nsToDatabase(c.getNS()));

    BSONArrayBuilder allUsers(builder.subarrayStart("allUsers"));
    auto name = authSession->getAuthenticatedUserName();
    if (name) {
        name->serializeToBSON(&allUsers);
    }
    allUsers.doneFast();

    builder.append("user", name ? name->getDisplayName() : "");
}

void OpDebug::appendDelinquentInfo(OperationContext* opCtx,
                                   BSONObjBuilder& bob,
                                   bool reportAcquisitions) {
    const auto& admCtx = ExecutionAdmissionContext::get(opCtx);
    if (reportAcquisitions && admCtx.getDelinquentAcquisitions() > 0) {
        bob.append("totalDelinquentAcquisitions", admCtx.getDelinquentAcquisitions());
        bob.append("totalAcquisitionDelinquencyMillis",
                   admCtx.getTotalAcquisitionDelinquencyMillis());
        bob.append("maxAcquisitionDelinquencyMillis", admCtx.getMaxAcquisitionDelinquencyMillis());
    }

    if (auto* stats = opCtx->overdueInterruptCheckStats();
        stats && stats->overdueInterruptChecks.loadRelaxed() > 0) {
        bob.append("overdueInterruptChecks", stats->overdueInterruptChecks.loadRelaxed());
        bob.append("overdueInterruptTotalMillis", stats->overdueAccumulator.loadRelaxed().count());
        bob.append("overdueInterruptApproxMaxMillis", stats->overdueMaxTime.loadRelaxed().count());
    }
}

std::function<BSONObj(ProfileFilter::Args)> OpDebug::appendStaged(OperationContext* opCtx,
                                                                  StringSet requestedFields,
                                                                  bool needWholeDocument) {
    // This function is analogous to OpDebug::append. The main difference is that append() does
    // the work of building BSON right away, while appendStaged() stages the work to be done
    // later. It returns a std::function that builds BSON when called.

    // The other difference is that appendStaged can avoid building BSON for unneeded fields.
    // requestedFields is a set of top-level field names; any fields beyond this list may be
    // omitted. This also lets us uassert if the caller asks for an unsupported field.

    // Each piece of the result is a function that appends to a BSONObjBuilder.
    // Before returning, we encapsulate the result in a simpler function that returns a BSONObj.
    using Piece = std::function<void(ProfileFilter::Args, BSONObjBuilder&)>;
    std::vector<Piece> pieces;

    // For convenience, the callback that handles each field gets the fieldName as an extra arg.
    using Callback = std::function<void(const char*, ProfileFilter::Args, BSONObjBuilder&)>;

    // Helper to check for the presence of a field in the StringSet, and remove it.
    // At the end of this method, anything left in the StringSet is a field we don't know
    // how to handle.
    auto needs = [&](const char* fieldName) {
        bool val = needWholeDocument || requestedFields.count(fieldName) > 0;
        requestedFields.erase(fieldName);
        return val;
    };
    auto addIfNeeded = [&](const char* fieldName, Callback cb) {
        if (needs(fieldName)) {
            pieces.push_back([fieldName = fieldName, cb = std::move(cb)](auto args, auto& b) {
                cb(fieldName, args, b);
            });
        }
    };

    addIfNeeded("ts", [](auto field, auto args, auto& b) { b.append(field, Date_t::now()); });
    addIfNeeded("client", [](auto field, auto args, auto& b) {
        b.append(field, args.opCtx->getClient()->clientAddress());
    });
    addIfNeeded("appName", [](auto field, auto args, auto& b) {
        if (auto clientMetadata = ClientMetadata::get(args.opCtx->getClient())) {
            auto appName = clientMetadata->getApplicationName();
            if (!appName.empty()) {
                b.append(field, appName);
            }
        }
    });
    bool needsAllUsers = needs("allUsers");
    bool needsUser = needs("user");
    if (needsAllUsers || needsUser) {
        pieces.push_back([](auto args, auto& b) {
            AuthorizationSession* authSession = AuthorizationSession::get(args.opCtx->getClient());
            appendUserInfo(args.curop, b, authSession);
        });
    }

    addIfNeeded("op", [](auto field, auto args, auto& b) {
        b.append(field, logicalOpToString(args.op.logicalOp));
    });
    addIfNeeded("ns", [](auto field, auto args, auto& b) { b.append(field, args.curop.getNS()); });

    addIfNeeded("command", [](auto field, auto args, auto& b) {
        curop_bson_helpers::appendObjectTruncatingAsNecessary(
            field,
            curop_bson_helpers::appendCommentField(args.opCtx, args.curop.opDescription()),
            appendMaxElementSize,
            b);
    });

    addIfNeeded("originatingCommand", [](auto field, auto args, auto& b) {
        auto originatingCommand = args.curop.originatingCommand();
        if (!originatingCommand.isEmpty()) {
            curop_bson_helpers::appendObjectTruncatingAsNecessary(
                field, originatingCommand, appendMaxElementSize, b);
        }
    });

    addIfNeeded("nShards", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_NUMBER2(b, field, args.op.nShards);
    });
    addIfNeeded("cursorid", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_NUMBER2(b, field, args.op.cursorid);
    });
    addIfNeeded("mongot", [](auto field, auto args, auto& b) {
        if (args.op.mongotCursorId) {
            b.append(field, args.op.makeMongotDebugStatsObject());
        }
    });
    addIfNeeded("exhaust", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.exhaust);
    });

    addIfNeeded("keysExamined", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.keysExamined);
    });
    addIfNeeded("docsExamined", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.docsExamined);
    });
    addIfNeeded("hasSortStage", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.additiveMetrics.hasSortStage);
    });
    addIfNeeded("usedDisk", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.additiveMetrics.usedDisk);
    });
    addIfNeeded("fromMultiPlanner", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.additiveMetrics.fromMultiPlanner);
    });
    addIfNeeded("fromPlanCache", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.additiveMetrics.fromPlanCache.value_or(false));
    });
    addIfNeeded("replanned", [](auto field, auto args, auto& b) {
        if (args.op.replanReason) {
            OPDEBUG_APPEND_BOOL2(b, field, true);
        }
    });
    addIfNeeded("replanReason", [](auto field, auto args, auto& b) {
        if (args.op.replanReason) {
            b.append(field, *args.op.replanReason);
        }
    });
    addIfNeeded("nMatched", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nMatched);
    });
    addIfNeeded("nBatches", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nBatches);
    });
    addIfNeeded("nModified", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nModified);
    });
    addIfNeeded("ninserted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.ninserted);
    });
    addIfNeeded("ndeleted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.ndeleted);
    });
    addIfNeeded("nUpserted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nUpserted);
    });
    addIfNeeded("cursorExhausted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_BOOL2(b, field, args.op.cursorExhausted);
    });

    addIfNeeded("keysInserted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.keysInserted);
    });
    addIfNeeded("keysDeleted", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.keysDeleted);
    });

    addIfNeeded("prepareReadConflicts", [](auto field, auto args, auto& b) {
        if (auto n = args.curop.getPrepareReadConflicts(); n > 0) {
            b.append(field, n);
        }
    });

    addIfNeeded("writeConflicts", [](auto field, auto args, auto& b) {
        const auto& storageMetrics = args.curop.getOperationStorageMetrics();

        if (storageMetrics.writeConflicts > 0) {
            b.append(field, storageMetrics.writeConflicts);
        }
    });

    addIfNeeded("temporarilyUnavailableErrors", [](auto field, auto args, auto& b) {
        const auto& storageMetrics = args.curop.getOperationStorageMetrics();

        if (storageMetrics.temporarilyUnavailableErrors > 0) {
            b.append(field, storageMetrics.temporarilyUnavailableErrors);
        }
    });

    addIfNeeded("dataThroughputLastSecond", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.dataThroughputLastSecond);
    });
    addIfNeeded("dataThroughputAverage", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.dataThroughputAverage);
    });

    addIfNeeded("numYield", [](auto field, auto args, auto& b) {
        b.appendNumber(field, args.curop.numYields());
    });
    addIfNeeded("nreturned", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.additiveMetrics.nreturned);
    });

    addIfNeeded("planCacheShapeHash", [](auto field, auto args, auto& b) {
        if (args.op.planCacheShapeHash) {
            b.append(field, zeroPaddedHex(*args.op.planCacheShapeHash));
        }
    });
    // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
    addIfNeeded("queryHash", [](auto field, auto args, auto& b) {
        if (args.op.planCacheShapeHash) {
            b.append(field, zeroPaddedHex(*args.op.planCacheShapeHash));
        }
    });
    addIfNeeded("planCacheKey", [](auto field, auto args, auto& b) {
        if (args.op.planCacheKey) {
            b.append(field, zeroPaddedHex(*args.op.planCacheKey));
        }
    });
    addIfNeeded("queryShapeHash", [&](auto field, auto args, auto& b) {
        if (auto&& hash = args.op.getQueryShapeHash()) {
            b.append(field, hash->toHexString());
        }
    });

    addIfNeeded("queryFramework", [](auto field, auto args, auto& b) {
        switch (args.op.queryFramework) {
            case PlanExecutor::QueryFramework::kClassicOnly:
            case PlanExecutor::QueryFramework::kClassicHybrid:
                b.append("queryFramework", "classic");
                break;
            case PlanExecutor::QueryFramework::kSBEOnly:
            case PlanExecutor::QueryFramework::kSBEHybrid:
                b.append("queryFramework", "sbe");
                break;
            case PlanExecutor::QueryFramework::kUnknown:
                break;
        }
    });

    addIfNeeded("locks", [](auto field, auto args, auto& b) {
        auto lockerInfo =
            shard_role_details::getLocker(args.opCtx)->getLockerInfo(args.curop.getLockStatsBase());
        BSONObjBuilder locks(b.subobjStart(field));
        lockerInfo.stats.report(&locks);
    });

    addIfNeeded("authorization", [](auto field, auto args, auto& b) {
        auto userAcquisitionStats = args.curop.getUserAcquisitionStats();
        if (userAcquisitionStats->shouldReportUserCacheAccessStats()) {
            BSONObjBuilder userCacheAcquisitionStatsBuilder(b.subobjStart(field));
            userAcquisitionStats->reportUserCacheAcquisitionStats(
                &userCacheAcquisitionStatsBuilder,
                args.opCtx->getServiceContext()->getTickSource());
        }

        if (userAcquisitionStats->shouldReportLDAPOperationStats()) {
            BSONObjBuilder ldapOperationStatsBuilder(b.subobjStart(field));
            userAcquisitionStats->reportLdapOperationStats(
                &ldapOperationStatsBuilder, args.opCtx->getServiceContext()->getTickSource());
        }
    });

    addIfNeeded("flowControl", [](auto field, auto args, auto& b) {
        BSONObj flowControlMetrics =
            makeFlowControlObject(shard_role_details::getLocker(args.opCtx)->getFlowControlStats());
        BSONObjBuilder flowControlBuilder(b.subobjStart(field));
        flowControlBuilder.appendElements(flowControlMetrics);
    });

    addIfNeeded("writeConcern", [](auto field, auto args, auto& b) {
        if (args.op.writeConcern && !args.op.writeConcern->usedDefaultConstructedWC) {
            b.append(field, args.op.writeConcern->toBSON());
        }
    });

    addIfNeeded("storage", [](auto field, auto args, auto& b) {
        if (args.op.storageStats) {
            b.append(field, args.op.storageStats->toBSON());
        }
    });

    addIfNeeded("spillStorage", [](auto field, auto args, auto& b) {
        if (args.op.spillStorageStats) {
            b.append(field, args.op.spillStorageStats->toBSON());
        }
    });

    // Don't short-circuit: call needs() for every supported field, so that at the end we can
    // uassert that no unsupported fields were requested.
    bool needsOk = needs("ok");
    bool needsErrMsg = needs("errMsg");
    bool needsErrName = needs("errName");
    bool needsErrCode = needs("errCode");
    if (needsOk || needsErrMsg || needsErrName || needsErrCode) {
        pieces.push_back([](auto args, auto& b) {
            if (!args.op.errInfo.isOK()) {
                b.appendNumber("ok", 0.0);
                if (!args.op.errInfo.reason().empty()) {
                    b.append("errMsg", args.op.errInfo.reason());
                }
                b.append("errName", ErrorCodes::errorString(args.op.errInfo.code()));
                b.append("errCode", args.op.errInfo.code());
            }
        });
    }

    addIfNeeded("responseLength", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_NUMBER2(b, field, args.op.responseLength);
    });

    addIfNeeded("protocol", [](auto field, auto args, auto& b) {
        if (args.op.iscommand) {
            b.append(field, getProtoString(args.op.networkOp));
        }
    });

    addIfNeeded("remoteOpWaitMillis", [](auto field, auto args, auto& b) {
        if (args.op.remoteOpWaitTime) {
            b.append(field, durationCount<Milliseconds>(*args.op.remoteOpWaitTime));
        }
    });

    addIfNeeded("cpuNanos", [](auto field, auto args, auto& b) {
        // Always report cpuNanos in rare cases that it is zero to facilitate testing that expects
        // this field to always exist.
        if (args.op.cpuTime >= Nanoseconds::zero()) {
            b.appendNumber(field, durationCount<Nanoseconds>(args.op.cpuTime));
        }
    });

    // millis and durationMillis are the same thing. This is one of the few inconsistencies between
    // the profiler (OpDebug::append) and the log file (OpDebug::report), so for the profile filter
    // we support both names.
    addIfNeeded("millis", [](auto field, auto args, auto& b) {
        b.appendNumber(field, durationCount<Milliseconds>(args.curop.elapsedTimeTotal()));
    });
    addIfNeeded("durationMillis", [](auto field, auto args, auto& b) {
        b.appendNumber(field, durationCount<Milliseconds>(args.curop.elapsedTimeTotal()));
    });

    addIfNeeded("workingMillis", [](auto field, auto args, auto& b) {
        b.appendNumber(field,
                       durationCount<Milliseconds>(
                           args.op.additiveMetrics.clusterWorkingTime.value_or(Milliseconds{0})));
    });

    addIfNeeded("planSummary", [](auto field, auto args, auto& b) {
        if (!args.curop.getPlanSummary().empty()) {
            b.append(field, args.curop.getPlanSummary());
        }
    });

    addIfNeeded("planningTimeMicros", [](auto field, auto args, auto& b) {
        b.appendNumber(field, durationCount<Microseconds>(args.op.planningTime));
    });

    addIfNeeded("estimatedCost", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.estimatedCost);
    });

    addIfNeeded("estimatedCardinality", [](auto field, auto args, auto& b) {
        OPDEBUG_APPEND_OPTIONAL(b, field, args.op.estimatedCardinality);
    });

    addIfNeeded("totalOplogSlotDurationMicros", [](auto field, auto args, auto& b) {
        if (auto totalOplogSlotDurationMicros =
                LocalOplogInfo::getOplogSlotTimeContext(args.opCtx).getTotalMicros();
            totalOplogSlotDurationMicros > Microseconds::zero()) {
            b.appendNumber(field, durationCount<Microseconds>(totalOplogSlotDurationMicros));
        }
    });

    addIfNeeded("execStats", [](auto field, auto args, auto& b) {
        if (!args.op.execStats.isEmpty()) {
            b.append(field, args.op.execStats);
        }
    });

    if (!requestedFields.empty()) {
        std::stringstream ss;
        ss << "No such field (or fields) available for profile filter";
        auto sep = ": ";
        for (auto&& s : requestedFields) {
            ss << sep << s;
            sep = ", ";
        }
        uasserted(4910200, ss.str());
    }

    return [pieces = std::move(pieces)](ProfileFilter::Args args) {
        BSONObjBuilder bob;
        for (const auto& piece : pieces) {
            piece(args, bob);
        }
        return bob.obj();
    };
}

void OpDebug::setPlanSummaryMetrics(PlanSummaryStats&& planSummaryStats) {
    // Data-bearing node metrics need to be aggregated here rather than just assigned.
    // Certain operations like $mergeCursors may have already accumulated metrics from remote
    // data-bearing nodes, and we need to add in the work done locally.
    additiveMetrics.keysExamined =
        additiveMetrics.keysExamined.value_or(0) + planSummaryStats.totalKeysExamined;
    additiveMetrics.docsExamined =
        additiveMetrics.docsExamined.value_or(0) + planSummaryStats.totalDocsExamined;
    additiveMetrics.hasSortStage = additiveMetrics.hasSortStage || planSummaryStats.hasSortStage;
    additiveMetrics.usedDisk = additiveMetrics.usedDisk || planSummaryStats.usedDisk;
    additiveMetrics.fromMultiPlanner =
        additiveMetrics.fromMultiPlanner || planSummaryStats.fromMultiPlanner;
    // Note that fromPlanCache is an AND of all operations rather than an OR like the other metrics.
    // This is to ensure we register when any part of the query _missed_ the cache, which is thought
    // to be the more interesting event.
    if (!additiveMetrics.fromPlanCache.has_value()) {
        additiveMetrics.fromPlanCache = true;
    }
    *additiveMetrics.fromPlanCache =
        *additiveMetrics.fromPlanCache && planSummaryStats.fromPlanCache;

    spillingStatsPerStage = std::move(planSummaryStats.spillingStatsPerStage);

    sortTotalDataSizeBytes = planSummaryStats.sortTotalDataSizeBytes;
    keysSorted = planSummaryStats.keysSorted;
    collectionScans = planSummaryStats.collectionScans;
    collectionScansNonTailable = planSummaryStats.collectionScansNonTailable;

    replanReason = std::move(planSummaryStats.replanReason);
    indexesUsed = std::move(planSummaryStats.indexesUsed);
}

BSONObj OpDebug::makeFlowControlObject(FlowControlTicketholder::CurOp stats) {
    BSONObjBuilder builder;
    if (stats.ticketsAcquired > 0) {
        builder.append("acquireCount", stats.ticketsAcquired);
    }

    if (stats.acquireWaitCount > 0) {
        builder.append("acquireWaitCount", stats.acquireWaitCount);
    }

    if (stats.timeAcquiringMicros > 0) {
        builder.append("timeAcquiringMicros", stats.timeAcquiringMicros);
    }

    return builder.obj();
}

BSONObj OpDebug::makeMongotDebugStatsObject() const {
    BSONObjBuilder cursorBuilder;
    invariant(mongotCursorId);
    cursorBuilder.append("cursorid", mongotCursorId.value());
    if (msWaitingForMongot) {
        cursorBuilder.append("timeWaitingMillis", msWaitingForMongot.value());
    }
    cursorBuilder.append("batchNum", mongotBatchNum);
    if (!mongotCountVal.isEmpty()) {
        cursorBuilder.append("resultCount", mongotCountVal);
    }
    if (!mongotSlowQueryLog.isEmpty()) {
        cursorBuilder.appendElements(mongotSlowQueryLog);
    }
    return cursorBuilder.obj();
}

void OpDebug::addResolvedViews(const std::vector<NamespaceString>& namespaces,
                               const std::vector<BSONObj>& pipeline) {
    if (namespaces.empty())
        return;

    if (resolvedViews.find(namespaces.front()) == resolvedViews.end()) {
        resolvedViews[namespaces.front()] = std::make_pair(namespaces, pipeline);
    }
}

static void appendResolvedViewsInfoImpl(
    BSONArrayBuilder& resolvedViewsArr,
    const std::map<NamespaceString, std::pair<std::vector<NamespaceString>, std::vector<BSONObj>>>&
        resolvedViews) {
    for (const auto& kv : resolvedViews) {
        const NamespaceString& viewNss = kv.first;
        const std::vector<NamespaceString>& dependencies = kv.second.first;
        const std::vector<BSONObj>& pipeline = kv.second.second;

        BSONObjBuilder aView;
        aView.append("viewNamespace",
                     NamespaceStringUtil::serialize(viewNss, SerializationContext::stateDefault()));

        BSONArrayBuilder dependenciesArr(aView.subarrayStart("dependencyChain"));
        for (const auto& nss : dependencies) {
            dependenciesArr.append(std::string{nss.coll()});
        }
        dependenciesArr.doneFast();

        BSONArrayBuilder pipelineArr(aView.subarrayStart("resolvedPipeline"));
        for (const auto& stage : pipeline) {
            pipelineArr.append(stage);
        }
        pipelineArr.doneFast();

        resolvedViewsArr.append(redact(aView.done()));
    }
}

CursorMetrics OpDebug::getCursorMetrics() const {
    CursorMetrics metrics;

    metrics.setKeysExamined(additiveMetrics.keysExamined.value_or(0));
    metrics.setDocsExamined(additiveMetrics.docsExamined.value_or(0));
    metrics.setBytesRead(additiveMetrics.bytesRead.value_or(0));

    metrics.setReadingTimeMicros(additiveMetrics.readingTime.value_or(Microseconds(0)).count());
    metrics.setWorkingTimeMillis(
        additiveMetrics.clusterWorkingTime.value_or(Milliseconds(0)).count());
    metrics.setCpuNanos(additiveMetrics.cpuNanos.value_or(Nanoseconds(0)).count());

    metrics.setDelinquentAcquisitions(additiveMetrics.delinquentAcquisitions.value_or(0));
    metrics.setTotalAcquisitionDelinquencyMillis(
        additiveMetrics.totalAcquisitionDelinquency.value_or(Milliseconds(0)).count());
    metrics.setMaxAcquisitionDelinquencyMillis(
        additiveMetrics.maxAcquisitionDelinquency.value_or(Milliseconds(0)).count());

    metrics.setNumInterruptChecks(additiveMetrics.numInterruptChecks.value_or(0));
    metrics.setOverdueInterruptApproxMaxMillis(
        additiveMetrics.overdueInterruptApproxMax.value_or(Milliseconds(0)).count());

    metrics.setHasSortStage(additiveMetrics.hasSortStage);
    metrics.setUsedDisk(additiveMetrics.usedDisk);
    metrics.setFromMultiPlanner(additiveMetrics.fromMultiPlanner);
    metrics.setFromPlanCache(additiveMetrics.fromPlanCache.value_or(false));

    return metrics;
}

boost::optional<query_shape::QueryShapeHash> OpDebug::getQueryShapeHash() const {
    // Access to OpDebug is already synchronized, therefore no extra lock is taken here.
    return _queryShapeHash;
}

/**
 * Convenience method that sets 'queryShapeHash' if 'queryShapeHash' has not been previously
 * set. Currently QueryShapeHash for a given command may be computed twice (due to view
 * resolution). By preventing new QueryShapeHash overwrites we ensure that original
 * QueryShapeHash is recorded in CurOp::OpDebug.
 */
void OpDebug::setQueryShapeHashIfNotPresent(
    OperationContext* opCtx, const boost::optional<query_shape::QueryShapeHash>& hash) {
    // Field 'queryShapeHash' is computed by the command handler but may be accessed by
    // $currentOp thread and therefore needs to be synchronized.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    if (!_queryShapeHash) {
        _queryShapeHash = hash;
    }
}

BSONArray OpDebug::getResolvedViewsInfo() const {
    BSONArrayBuilder resolvedViewsArr;
    appendResolvedViewsInfoImpl(resolvedViewsArr, this->resolvedViews);
    return resolvedViewsArr.arr();
}

void OpDebug::appendResolvedViewsInfo(BSONObjBuilder& builder) const {
    BSONArrayBuilder resolvedViewsArr(builder.subarrayStart("resolvedViews"));
    appendResolvedViewsInfoImpl(resolvedViewsArr, this->resolvedViews);
    resolvedViewsArr.doneFast();
}

std::string OpDebug::getCollectionType(const NamespaceString& nss) const {
    if (nss.isEmpty()) {
        return "none";
    } else if (!resolvedViews.empty()) {
        auto dependencyItr = resolvedViews.find(nss);
        // 'resolvedViews' might be populated if any other collection as a part of the query is on a
        // view. However, it will not have associated dependencies.
        if (dependencyItr == resolvedViews.end()) {
            return "normal";
        }
        const std::vector<NamespaceString>& dependencies = dependencyItr->second.first;

        auto nssIterInDeps = std::find(dependencies.begin(), dependencies.end(), nss);
        tassert(7589000,
                str::stream() << "The view with ns: " << nss.toStringForErrorMsg()
                              << ", should have a valid dependency.",
                nssIterInDeps != (dependencies.end() - 1) && nssIterInDeps != dependencies.end());

        // The underlying namespace for the view/timeseries collection is the next namespace in the
        // dependency chain. If the view depends on a timeseries buckets collection, then it is a
        // timeseries collection, otherwise it is a regular view.
        const NamespaceString& underlyingNss = *std::next(nssIterInDeps);
        if (underlyingNss.isTimeseriesBucketsCollection()) {
            return "timeseries";
        }
        return "view";
    } else if (nss.isTimeseriesBucketsCollection()) {
        return "timeseriesBuckets";
    } else if (nss.isSystem()) {
        return "system";
    } else if (nss.isConfigDB()) {
        return "config";
    } else if (nss.isAdminDB()) {
        return "admin";
    } else if (nss.isLocalDB()) {
        return "local";
    } else if (nss.isNormalCollection()) {
        return "normal";
    }
    return "unknown";
}

namespace {

/**
 * Adds two boost::optionals of the same type with an operator+() together. Returns boost::none if
 * both 'lhs' and 'rhs' are uninitialized, or the sum of 'lhs' and 'rhs' if they are both
 * initialized. Returns 'lhs' if only 'rhs' is uninitialized and vice-versa.
 */
template <typename T>
boost::optional<T> addOptionals(const boost::optional<T>& lhs, const boost::optional<T>& rhs) {
    if (!rhs) {
        return lhs;
    }
    return lhs ? (*lhs + *rhs) : rhs;
}
}  // namespace

void OpDebug::AdditiveMetrics::add(const AdditiveMetrics& otherMetrics) {
    keysExamined = addOptionals(keysExamined, otherMetrics.keysExamined);
    docsExamined = addOptionals(docsExamined, otherMetrics.docsExamined);
    bytesRead = addOptionals(bytesRead, otherMetrics.bytesRead);
    nMatched = addOptionals(nMatched, otherMetrics.nMatched);
    nreturned = addOptionals(nreturned, otherMetrics.nreturned);
    nBatches = addOptionals(nBatches, otherMetrics.nBatches);
    nModified = addOptionals(nModified, otherMetrics.nModified);
    ninserted = addOptionals(ninserted, otherMetrics.ninserted);
    ndeleted = addOptionals(ndeleted, otherMetrics.ndeleted);
    nUpserted = addOptionals(nUpserted, otherMetrics.nUpserted);
    keysInserted = addOptionals(keysInserted, otherMetrics.keysInserted);
    keysDeleted = addOptionals(keysDeleted, otherMetrics.keysDeleted);
    readingTime = addOptionals(readingTime, otherMetrics.readingTime);
    clusterWorkingTime = addOptionals(clusterWorkingTime, otherMetrics.clusterWorkingTime);
    cpuNanos = addOptionals(cpuNanos, otherMetrics.cpuNanos);
    executionTime = addOptionals(executionTime, otherMetrics.executionTime);

    numInterruptChecks = addOptionals(numInterruptChecks, otherMetrics.numInterruptChecks);
    if (otherMetrics.overdueInterruptApproxMax.has_value()) {
        overdueInterruptApproxMax = std::max(overdueInterruptApproxMax.value_or(Milliseconds(0)),
                                             *otherMetrics.overdueInterruptApproxMax);
    }

    delinquentAcquisitions =
        addOptionals(delinquentAcquisitions, otherMetrics.delinquentAcquisitions);
    totalAcquisitionDelinquency =
        addOptionals(totalAcquisitionDelinquency, otherMetrics.totalAcquisitionDelinquency);
    if (otherMetrics.maxAcquisitionDelinquency.has_value()) {
        maxAcquisitionDelinquency = std::max(maxAcquisitionDelinquency.value_or(Milliseconds(0)),
                                             *otherMetrics.maxAcquisitionDelinquency);
    }

    hasSortStage = hasSortStage || otherMetrics.hasSortStage;
    usedDisk = usedDisk || otherMetrics.usedDisk;
    fromMultiPlanner = fromMultiPlanner || otherMetrics.fromMultiPlanner;
    // Note that fromPlanCache is an AND of all operations rather than an OR like the other metrics.
    // This is to ensure we register when any part of the query _missed_ the cache, which is thought
    // to be the more interesting event.
    if (!fromPlanCache.has_value()) {
        fromPlanCache = true;
    }
    *fromPlanCache = *fromPlanCache && otherMetrics.fromPlanCache.value_or(true);
}

void OpDebug::AdditiveMetrics::aggregateDataBearingNodeMetrics(
    const query_stats::DataBearingNodeMetrics& metrics) {
    keysExamined = keysExamined.value_or(0) + metrics.keysExamined;
    docsExamined = docsExamined.value_or(0) + metrics.docsExamined;
    bytesRead = bytesRead.value_or(0) + metrics.bytesRead;
    readingTime = readingTime.value_or(Microseconds(0)) + metrics.readingTime;
    clusterWorkingTime = clusterWorkingTime.value_or(Milliseconds(0)) + metrics.clusterWorkingTime;
    cpuNanos = cpuNanos.value_or(Nanoseconds(0)) + metrics.cpuNanos;

    delinquentAcquisitions = delinquentAcquisitions.value_or(0) + metrics.delinquentAcquisitions;
    totalAcquisitionDelinquency =
        totalAcquisitionDelinquency.value_or(Milliseconds(0)) + metrics.totalAcquisitionDelinquency;
    maxAcquisitionDelinquency = std::max(maxAcquisitionDelinquency.value_or(Milliseconds(0)),
                                         metrics.maxAcquisitionDelinquency);

    numInterruptChecks = numInterruptChecks.value_or(0) + metrics.numInterruptChecks;
    overdueInterruptApproxMax = std::max(overdueInterruptApproxMax.value_or(Milliseconds(0)),
                                         metrics.overdueInterruptApproxMax);

    hasSortStage = hasSortStage || metrics.hasSortStage;
    usedDisk = usedDisk || metrics.usedDisk;
    fromMultiPlanner = fromMultiPlanner || metrics.fromMultiPlanner;
    // Note that fromPlanCache is an AND of all operations rather than an OR like the other metrics.
    // This is to ensure we register when any part of the query _missed_ the cache, which is thought
    // to be the more interesting event.
    if (!fromPlanCache.has_value()) {
        fromPlanCache = true;
    }
    *fromPlanCache = *fromPlanCache && metrics.fromPlanCache;
}

void OpDebug::AdditiveMetrics::aggregateDataBearingNodeMetrics(
    const boost::optional<query_stats::DataBearingNodeMetrics>& metrics) {
    if (metrics) {
        aggregateDataBearingNodeMetrics(*metrics);
    }
}

void OpDebug::AdditiveMetrics::aggregateCursorMetrics(const CursorMetrics& metrics) {
    aggregateDataBearingNodeMetrics(query_stats::DataBearingNodeMetrics{
        static_cast<uint64_t>(metrics.getKeysExamined()),
        static_cast<uint64_t>(metrics.getDocsExamined()),
        static_cast<uint64_t>(metrics.getBytesRead()),
        Microseconds(metrics.getReadingTimeMicros()),
        Milliseconds(metrics.getWorkingTimeMillis()),
        Nanoseconds(metrics.getCpuNanos()),
        static_cast<uint64_t>(metrics.getDelinquentAcquisitions()),
        Milliseconds(metrics.getTotalAcquisitionDelinquencyMillis()),
        Milliseconds(metrics.getMaxAcquisitionDelinquencyMillis()),
        static_cast<uint64_t>(metrics.getNumInterruptChecks()),
        Milliseconds(metrics.getOverdueInterruptApproxMaxMillis()),
        metrics.getHasSortStage(),
        metrics.getUsedDisk(),
        metrics.getFromMultiPlanner(),
        metrics.getFromPlanCache()});
}

void OpDebug::AdditiveMetrics::aggregateStorageStats(const StorageStats& stats) {
    bytesRead = bytesRead.value_or(0) + stats.bytesRead();
    readingTime = readingTime.value_or(Microseconds(0)) + stats.readingTime();
}

void OpDebug::AdditiveMetrics::reset() {
    keysExamined = boost::none;
    docsExamined = boost::none;
    nMatched = boost::none;
    nreturned = boost::none;
    nBatches = boost::none;
    nModified = boost::none;
    ninserted = boost::none;
    ndeleted = boost::none;
    nUpserted = boost::none;
    keysInserted = boost::none;
    keysDeleted = boost::none;
    executionTime = boost::none;
}

bool OpDebug::AdditiveMetrics::equals(const AdditiveMetrics& otherMetrics) const {
    return keysExamined == otherMetrics.keysExamined && docsExamined == otherMetrics.docsExamined &&
        nMatched == otherMetrics.nMatched && nreturned == otherMetrics.nreturned &&
        nBatches == otherMetrics.nBatches && nModified == otherMetrics.nModified &&
        ninserted == otherMetrics.ninserted && ndeleted == otherMetrics.ndeleted &&
        nUpserted == otherMetrics.nUpserted && keysInserted == otherMetrics.keysInserted &&
        keysDeleted == otherMetrics.keysDeleted && executionTime == otherMetrics.executionTime;
}

void OpDebug::AdditiveMetrics::incrementKeysInserted(long long n) {
    if (!keysInserted) {
        keysInserted = 0;
    }
    *keysInserted += n;
}

void OpDebug::AdditiveMetrics::incrementKeysDeleted(long long n) {
    if (!keysDeleted) {
        keysDeleted = 0;
    }
    *keysDeleted += n;
}

void OpDebug::AdditiveMetrics::incrementNreturned(long long n) {
    if (!nreturned) {
        nreturned = 0;
    }
    *nreturned += n;
}

void OpDebug::AdditiveMetrics::incrementNBatches() {
    if (!nBatches) {
        nBatches = 0;
    }
    ++(*nBatches);
}

void OpDebug::AdditiveMetrics::incrementNinserted(long long n) {
    if (!ninserted) {
        ninserted = 0;
    }
    *ninserted += n;
}

void OpDebug::AdditiveMetrics::incrementNdeleted(long long n) {
    if (!ndeleted) {
        ndeleted = 0;
    }
    *ndeleted += n;
}

void OpDebug::AdditiveMetrics::incrementNUpserted(long long n) {
    if (!nUpserted) {
        nUpserted = 0;
    }
    *nUpserted += n;
}

std::string OpDebug::AdditiveMetrics::report() const {
    StringBuilder s;

    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysExamined", keysExamined);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("docsExamined", docsExamined);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nMatched", nMatched);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nreturned", nreturned);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nBatches", nBatches);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nModified", nModified);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ninserted", ninserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("ndeleted", ndeleted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("nUpserted", nUpserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysInserted", keysInserted);
    OPDEBUG_TOSTRING_HELP_OPTIONAL("keysDeleted", keysDeleted);
    if (executionTime) {
        s << " durationMillis:" << durationCount<Milliseconds>(*executionTime);
    }

    return s.str();
}

void OpDebug::AdditiveMetrics::report(logv2::DynamicAttributes* pAttrs) const {
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysExamined", keysExamined);
    OPDEBUG_TOATTR_HELP_OPTIONAL("docsExamined", docsExamined);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nMatched", nMatched);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nreturned", nreturned);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nBatches", nBatches);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nModified", nModified);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ninserted", ninserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("ndeleted", ndeleted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("nUpserted", nUpserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysInserted", keysInserted);
    OPDEBUG_TOATTR_HELP_OPTIONAL("keysDeleted", keysDeleted);
    if (executionTime) {
        pAttrs->add("durationMillis", durationCount<Milliseconds>(*executionTime));
    }
}

BSONObj OpDebug::AdditiveMetrics::reportBSON() const {
    BSONObjBuilder b;
    OPDEBUG_APPEND_OPTIONAL(b, "keysExamined", keysExamined);
    OPDEBUG_APPEND_OPTIONAL(b, "docsExamined", docsExamined);
    OPDEBUG_APPEND_OPTIONAL(b, "nMatched", nMatched);
    OPDEBUG_APPEND_OPTIONAL(b, "nreturned", nreturned);
    OPDEBUG_APPEND_OPTIONAL(b, "nBatches", nBatches);
    OPDEBUG_APPEND_OPTIONAL(b, "nModified", nModified);
    OPDEBUG_APPEND_OPTIONAL(b, "ninserted", ninserted);
    OPDEBUG_APPEND_OPTIONAL(b, "ndeleted", ndeleted);
    OPDEBUG_APPEND_OPTIONAL(b, "nUpserted", nUpserted);
    OPDEBUG_APPEND_OPTIONAL(b, "keysInserted", keysInserted);
    OPDEBUG_APPEND_OPTIONAL(b, "keysDeleted", keysDeleted);
    if (executionTime) {
        b.appendNumber("durationMillis", durationCount<Milliseconds>(*executionTime));
    }
    return b.obj();
}
}  // namespace mongo
