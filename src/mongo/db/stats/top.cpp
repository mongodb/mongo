// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/top.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/operation_latency_histogram.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

const auto getTop = ServiceContext::declareDecoration<Top>();
const auto getServiceLatencyTracker = Service::declareDecoration<ServiceLatencyTracker>();

// OTel histogram mirroring opLatencies.reads/.writes/.commands in serverStatus.
// Records elapsedTimeExcludingPauses() in microseconds, broken down by op_type.
// Bucket boundaries are derived from OperationLatencyHistogram's lower bounds (same edges),
// so bucket populations are closely comparable. Note a minor semantic difference: OTel uses
// inclusive upper bounds (e.g. (-inf, 2]), while OperationLatencyHistogram uses exclusive
// upper bounds (e.g. [0, 2)), so a value exactly equal to a boundary (e.g. 2 µs) lands in
// different buckets in the two histograms. For monitoring purposes this is negligible.
const otel::metrics::AttributeDefinition<std::string_view> kOpTypeAttrDef{
    .name = "op_type", .values = {"read"sv, "write"sv, "command"sv}};

// Build explicit OTel bucket boundaries from OperationLatencyHistogram's lower bounds.
// We use lower bounds [1..N-1] as OTel upper bounds, giving the same bucket edges.
// Lower bound [0] (value 0) is omitted: OTel's implicit first bucket covers (-inf, 2].
std::vector<double> makeOperationLatencyBucketBoundaries() {
    const auto& lowerBounds = operation_latency_histogram_details::getLowerBounds();
    std::vector<double> boundaries;
    invariant(!lowerBounds.empty(), "OperationLatencyHistogram lower bounds must be non-empty");
    boundaries.reserve(lowerBounds.size() - 1);
    for (size_t i = 1; i < lowerBounds.size(); ++i) {
        boundaries.push_back(static_cast<double>(lowerBounds[i]));
    }
    return boundaries;
}

otel::metrics::Histogram<int64_t, std::string_view>& operationLatencyHistogram =
    otel::metrics::MetricsService::instance().createInt64Histogram<std::string_view>(
        otel::metrics::MetricNames::kOperationLatency,
        "Wall-clock time of completed user operations excluding storage-engine yield time, "
        "broken down by operation type (read/write/command).",
        otel::metrics::MetricUnit::kMicroseconds,
        kOpTypeAttrDef,
        {.explicitBucketBoundaries = makeOperationLatencyBucketBoundaries()});

// Returns the op_type attribute string for OTel recording, or nullopt for op types excluded from
// this histogram. kTransaction is tracked separately via incrementForTransaction and is
// intentionally excluded here; if it should appear in future, add it in incrementForTransaction.
std::optional<std::string_view> opTypeString(Command::ReadWriteType rwType) {
    switch (rwType) {
        case Command::ReadWriteType::kRead:
            return "read"sv;
        case Command::ReadWriteType::kWrite:
            return "write"sv;
        case Command::ReadWriteType::kCommand:
            return "command"sv;
        case Command::ReadWriteType::kTransaction:
            return std::nullopt;
        default:
            MONGO_UNREACHABLE_TASSERT(12445300);
    }
}

// Returns true if the operation came from a network client connection (connectionId > 0)
// and is not running under DBDirectClient.
// Note: QE (Queryable Encryption) operations ARE included here; the isQE flag is passed separately
// to OperationLatencyHistogram::increment so that queryableEncryptionLatencyMicros is tracked
// correctly.
bool isUserConnection(OperationContext* opCtx) {
    auto* c = opCtx->getClient();
    return c->isFromUserConnection() && !c->isInDirectClient();
}

template <typename HistogramType>
void incrementHistogram(OperationContext* opCtx,
                        long long latency,
                        HistogramType& histogram,
                        Command::ReadWriteType readWriteType) {
    histogram.increment(latency,
                        readWriteType,
                        CurOp::shouldCurOpStackOmitDiagnosticInformation(CurOp::get(opCtx)));
}

template <typename HistogramType>
void incrementHistogramForUser(OperationContext* opCtx,
                               long long latency,
                               HistogramType& histogram,
                               Command::ReadWriteType readWriteType) {
    if (!isUserConnection(opCtx))
        return;
    incrementHistogram(opCtx, latency, histogram, readWriteType);
}

void updateCollectionData(OperationContext* opCtx,
                          Top::CollectionData& c,
                          LogicalOp logicalOp,
                          Top::LockType lockType,
                          long long micros,
                          Command::ReadWriteType readWriteType) {
    // isStatsRecordingAllowed is sticky-false: once any op observes
    // shouldOmitDiagnosticInformation, it stays false. Concurrent writers all converge to false
    // idempotently — there is no false→true transition, so a relaxed CAS-style
    // read-then-conditional-store is safe.
    if (c.isStatsRecordingAllowed.loadRelaxed()) {
        if (CurOp::get(opCtx)->getShouldOmitDiagnosticInformation()) {
            c.isStatsRecordingAllowed.storeRelaxed(false);
        }
    }

    incrementHistogramForUser(opCtx, micros, c.opLatencyHistogram, readWriteType);

    c.total.inc(micros);

    if (lockType == Top::LockType::WriteLocked)
        c.writeLock.inc(micros);
    else if (lockType == Top::LockType::ReadLocked)
        c.readLock.inc(micros);

    switch (logicalOp) {
        case LogicalOp::opInvalid:
            // use 0 for unknown, non-specific
            break;
        case LogicalOp::opBulkWrite:
            break;
        case LogicalOp::opUpdate:
            c.update.inc(micros);
            break;
        case LogicalOp::opInsert:
            c.insert.inc(micros);
            break;
        case LogicalOp::opQuery:
            c.queries.inc(micros);
            break;
        case LogicalOp::opGetMore:
            c.getmore.inc(micros);
            break;
        case LogicalOp::opDelete:
            c.remove.inc(micros);
            break;
        case LogicalOp::opKillCursors:
            break;
        case LogicalOp::opCommand:
            c.commands.inc(micros);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

ServiceLatencyTracker::ServiceLatencyTracker()
    : _totalTime({.includeEmptyBuckets = true, .logBucketScalingFactor = 3}) {}

ServiceLatencyTracker& ServiceLatencyTracker::getDecoration(Service* service) {
    return getServiceLatencyTracker(service);
}

void ServiceLatencyTracker::increment(OperationContext* opCtx,
                                      Microseconds latency,
                                      Microseconds workingTime,
                                      Command::ReadWriteType readWriteType) {
    if (!opCtx->shouldIncrementLatencyStats())
        return;

    if (!isUserConnection(opCtx))
        return;

    const bool isQE = CurOp::shouldCurOpStackOmitDiagnosticInformation(CurOp::get(opCtx));
    auto latencyCount = durationCount<Microseconds>(latency);
    auto workingTimeCount = durationCount<Microseconds>(workingTime);
    _totalTime.increment(latencyCount, readWriteType, isQE);
    _workingTime.increment(workingTimeCount, readWriteType, isQE);
    // OTel histogram records non-QE user ops only. QE ops are intentionally excluded:
    // their latency is tracked via opLatencies.*.queryableEncryptionLatencyMicros instead.
    if (!isQE) {
        if (auto opType = opTypeString(readWriteType)) {
            operationLatencyHistogram.record(latencyCount, {*opType});
        }
    }
}

void ServiceLatencyTracker::appendTotalTimeStats(bool includeHistograms,
                                                 bool slowMSBucketsOnly,
                                                 BSONObjBuilder* builder) {
    _totalTime.append(includeHistograms, slowMSBucketsOnly, builder);
}

void ServiceLatencyTracker::appendWorkingTimeStats(bool includeHistograms,
                                                   bool slowMSBucketsOnly,
                                                   BSONObjBuilder* builder) {
    _workingTime.append(includeHistograms, slowMSBucketsOnly, builder);
}

void ServiceLatencyTracker::incrementForTransaction(OperationContext* opCtx, Microseconds latency) {
    auto latencyCount = durationCount<Microseconds>(latency);
    incrementHistogram(opCtx, latencyCount, _totalTime, Command::ReadWriteType::kTransaction);
}

Top& Top::getDecoration(OperationContext* opCtx) {
    invariant(opCtx->getService()->role().hasExclusively(ClusterRole::ShardServer));
    return getTop(opCtx->getServiceContext());
}

void Top::record(OperationContext* opCtx,
                 const NamespaceString& nss,
                 LogicalOp logicalOp,
                 LockType lockType,
                 Microseconds micros,
                 bool command,
                 Command::ReadWriteType readWriteType) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    if (nssStr[0] == '?')
        return;

    auto hashedNs = UsageMap::hasher().hashed_key(nssStr);
    auto microsCount = durationCount<Microseconds>(micros);
    _withCollectionData(hashedNs, [&](CollectionData& coll) {
        updateCollectionData(opCtx, coll, logicalOp, lockType, microsCount, readWriteType);
    });
}

void Top::record(OperationContext* opCtx,
                 std::span<const NamespaceString> nssSet,
                 LogicalOp logicalOp,
                 LockType lockType,
                 Microseconds micros,
                 bool command,
                 Command::ReadWriteType readWriteType) {
    std::vector<std::string> hashedSet;
    hashedSet.reserve(nssSet.size());
    for (auto& nss : nssSet) {
        const auto nssStr =
            NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
        if (nssStr[0] != '?') {
            hashedSet.emplace_back(UsageMap::hasher().hashed_key(nssStr));
        }
    }

    auto microsCount = durationCount<Microseconds>(micros);

    // Open-coded version of _withCollectionData so the shared lock is acquired once for
    // the whole batch instead of per-namespace. Don't replace with per-key calls to the
    // helper without re-benchmarking — N shared-lock acquires cost more than this loop.
    std::vector<size_t> missing;
    {
        std::shared_lock lk(_lockUsage);  // NOLINT
        for (size_t i = 0; i < hashedSet.size(); ++i) {
            auto it = _usage.find(hashedSet[i]);
            if (it != _usage.end()) {
                updateCollectionData(
                    opCtx, *it->second, logicalOp, lockType, microsCount, readWriteType);
            } else {
                missing.push_back(i);
            }
        }
    }

    if (!missing.empty()) {
        std::lock_guard lk(_lockUsage);
        for (auto idx : missing) {
            auto& entry = _usage[hashedSet[idx]];
            if (!entry) {
                entry = std::make_unique<CollectionData>();
            }
            updateCollectionData(opCtx, *entry, logicalOp, lockType, microsCount, readWriteType);
        }
    }
}

void Top::collectionDropped(const NamespaceString& nss) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    std::lock_guard lk(_lockUsage);
    _usage.erase(nssStr);
}

void Top::appendStatsEntry(BSONObjBuilder& b, std::string_view name, const UsageData& data) {
    BSONObjBuilder bb(b.subobjStart(name));
    bb.appendNumber("time", data.time.loadRelaxed());
    bb.appendNumber("count", data.count.loadRelaxed());
    bb.done();
}

void Top::appendUsageStatsForCollection(BSONObjBuilder& result, const CollectionData& coll) {
    appendStatsEntry(result, "total", coll.total);

    appendStatsEntry(result, "readLock", coll.readLock);
    appendStatsEntry(result, "writeLock", coll.writeLock);

    appendStatsEntry(result, "queries", coll.queries);
    appendStatsEntry(result, "getmore", coll.getmore);
    appendStatsEntry(result, "insert", coll.insert);
    appendStatsEntry(result, "update", coll.update);
    appendStatsEntry(result, "remove", coll.remove);
    appendStatsEntry(result, "commands", coll.commands);
}

void Top::append(BSONObjBuilder& topStatsBuilder) {
    std::shared_lock lk(_lockUsage);  // NOLINT

    // Pull all the names into a vector so we can sort them for the user.
    std::vector<std::string> names;
    for (UsageMap::const_iterator i = _usage.begin(); i != _usage.end(); ++i) {
        names.push_back(i->first);
    }

    std::sort(names.begin(), names.end());

    for (size_t i = 0; i < names.size(); i++) {
        BSONObjBuilder bb(topStatsBuilder.subobjStart(names[i]));

        const CollectionData& coll = *_usage.find(names[i])->second;
        auto pos = names[i].find('.');
        if (coll.isStatsRecordingAllowed.loadRelaxed() &&
            !NamespaceString::isFLE2StateCollection(names[i].substr(pos + 1))) {
            appendUsageStatsForCollection(topStatsBuilder, coll);
        }
        bb.done();
    }
}

void Top::appendLatencyStats(const NamespaceString& nss,
                             bool includeHistograms,
                             BSONObjBuilder* builder) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    auto hashedNs = UsageMap::hasher().hashed_key(nssStr);
    _withCollectionData(hashedNs, [&](const CollectionData& coll) {
        BSONObjBuilder latencyStatsBuilder;
        coll.opLatencyHistogram.append(includeHistograms, false, &latencyStatsBuilder);
        builder->append("ns", nssStr);
        builder->append("latencyStats", latencyStatsBuilder.obj());
    });
}

void Top::appendOperationStats(const NamespaceString& nss, BSONObjBuilder* builder) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    auto hashedNs = UsageMap::hasher().hashed_key(nssStr);
    _withCollectionData(hashedNs, [&](const CollectionData& coll) {
        BSONObjBuilder opStatsBuilder;
        auto pos = nssStr.find('.');
        if (coll.isStatsRecordingAllowed.loadRelaxed() &&
            !NamespaceString::isFLE2StateCollection(nssStr.substr(pos + 1))) {
            appendUsageStatsForCollection(opStatsBuilder, coll);
        }
        builder->append("ns", nssStr);
        builder->append("operationStats", opStatsBuilder.obj());
    });
}
}  // namespace mongo
