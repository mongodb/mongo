// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/query_memory_load_shedding.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/memory_tracking/query_memory_load_shedding_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_lifespan.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/util/aligned.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <numbers>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

// Process-wide load-shedding state, published by the RSS monitor and read at query checkpoints.
struct QueryMemoryLoadSheddingState {
    // Read on every interrupt check; written rarely (rssBytes by the monitor ~10x/s, memLimitBytes
    // once). Kept together as read-mostly data.
    AtomicWord<int64_t> rssBytes{-1};  // -1: monitor has not published a sample yet (no signal).
    AtomicWord<int64_t> memLimitBytes{0};

    // Written (fetchAndAdd) on every shed. On its own cache line so those writes don't invalidate
    // the read-hot rssBytes/memLimitBytes line for the interrupt-check path (avoids false sharing).
    CacheExclusive<AtomicWord<long long>> operationsShed{0};
};
const auto getState = ServiceContext::declareDecoration<QueryMemoryLoadSheddingState>();

// Per-operation load-shedding bookkeeping. Scoped to the query's QueryLifespan (not the
// OperationContext) so it is set once on the originating command and survives that query's getMores
// and idle periods.
struct LoadSheddingOpState {
    // Opt-in eligibility (see is/markOperationQueryMemorySheddingEligible). Defaults false.
    bool eligible = false;
    // Wall-clock time of the previous shed evaluation, used to compute dt. Date_t::max() is the
    // "no dt baseline yet" sentinel: set below the low mark and before the first evaluation after
    // crossing above it, so the first post-crossing check only primes the clock instead of
    // computing a bogus dt.
    Date_t lastEvalTime = Date_t::max();
};
const auto getOpState = QueryLifespan::declareOpCtxDecoration<LoadSheddingOpState>();

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitQueryMemoryLoadShedding", [](ServiceContext* ctx) {
        getState(ctx).memLimitBytes.store(static_cast<int64_t>(ProcessInfo::getMemSizeBytes()));
    }};

// Minimum spacing between shed evaluations for one operation, to bound cost on the hot interrupt
// path. Skipped time accrues into the next dt, preserving the time-based probability.
constexpr Milliseconds kMinShedEvalInterval{1};

// Holds the periodic monitor job for the lifetime of the ServiceContext.
struct QueryMemoryRssMonitorHolder {
    std::mutex mutex;
    std::shared_ptr<PeriodicJobAnchor> anchor;
};
const auto getMonitorHolder = ServiceContext::declareDecoration<QueryMemoryRssMonitorHolder>();

// Test-only: overrides the RSS signal. Data {usagePercent: <int>} = percent of the memory limit.
MONGO_FAIL_POINT_DEFINE(queryMemoryPressureOverride);

// Test-only: forces the probabilistic roll to shed any otherwise-eligible operation.
MONGO_FAIL_POINT_DEFINE(queryMemoryLoadSheddingAlwaysShed);

bool loadSheddingEnabled() {
    return gQueryMemoryLoadSheddingLowMarkPercent.loadRelaxed() >= 0;
}

// Shed only on data-bearing processes (shards, config servers, standalone/replica-set mongods).
bool loadSheddingSupportedOnThisProcess() {
    return !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer);
}

// The current RSS signal in bytes, honoring the test-only override. Returns -1 when no signal.
int64_t effectiveRssBytes(const QueryMemoryLoadSheddingState& state) {
    if (auto sfp = queryMemoryPressureOverride.scoped(); MONGO_unlikely(sfp.isActive())) {
        const long long pct = sfp.getData()["usagePercent"].safeNumberLong();
        return state.memLimitBytes.loadRelaxed() * pct / 100;
    }
    return state.rssBytes.loadRelaxed();
}

// Export-only OTel mirror of operationsShed for fleet-wide monitoring; the in-process atomic
// remains the source of truth for serverStatus (opcounters pattern). RSS and the memory limit are
// not mirrored: process RSS is already serverStatus().mem.resident, and the memory limit is
// available via hostInfo, so a dashboard can compute RSS-vs-limit from those. Created lazily and
// stashed on first use (after MetricsService is up).
otel::metrics::Counter<int64_t>& operationsShedCounter() {
    static auto& counter = otel::metrics::MetricsService::instance().createInt64Counter(
        otel::metrics::MetricNames::kQueryMemoryLoadSheddingOperationsShed,
        "Operations load-shed due to query-memory pressure",
        otel::metrics::MetricUnit::kOperations);
    return counter;
}

// Samples the process RSS once and publishes it as the current signal.
void publishRssSample(ServiceContext* serviceContext) {
    const int64_t rssBytes = static_cast<int64_t>(ProcessInfo{}.getResidentSize()) * 1024 * 1024;
    getState(serviceContext).rssBytes.store(rssBytes);
}

}  // namespace

Status validateQueryMemoryLoadSheddingLowMark(std::int32_t lowMarkPercent,
                                              const boost::optional<TenantId>&) {
    // -1 disables the feature, so there is no relationship to enforce.
    if (lowMarkPercent >= 0 && lowMarkPercent >= gQueryMemoryLoadSheddingHighMarkPercent.load()) {
        return {ErrorCodes::BadValue,
                "queryMemoryLoadSheddingLowMarkPercent must be less than "
                "queryMemoryLoadSheddingHighMarkPercent when load shedding is enabled"};
    }
    return Status::OK();
}

Status validateQueryMemoryLoadSheddingHighMark(std::int32_t highMarkPercent,
                                               const boost::optional<TenantId>&) {
    const std::int32_t low = gQueryMemoryLoadSheddingLowMarkPercent.load();
    if (low >= 0 && highMarkPercent <= low) {
        return {ErrorCodes::BadValue,
                "queryMemoryLoadSheddingHighMarkPercent must be greater than "
                "queryMemoryLoadSheddingLowMarkPercent when load shedding is enabled"};
    }
    return Status::OK();
}

namespace query_memory_load_shedding_detail {

double shedProbability(int64_t rssBytes,
                       int64_t memLimitBytes,
                       int64_t opTrackedBytes,
                       int32_t lowMarkPercent,
                       int32_t highMarkPercent,
                       int64_t sizeReferenceBytes,
                       Milliseconds sinceLastCheck) {
    // Per-check shed probability, modeled as a Poisson process.
    // clang-format off
    //   pressure = clamp((rss/memLimit*100 - lowPct) / (highPct - lowPct), 0, 1)
    //   size     = opBytes / sizeRefBytes                    (unbounded above)
    //   hazard   = ln(2) * size * pressure / (1 - pressure)  (per-sec rate; diverges at high mark)
    //   p        = 1 - exp(-hazard * dtSeconds)
    // clang-format on
    // p is the probability of at least one shed event within dtSeconds -- the closed-form survival
    // of a Poisson process (P(N=0) = exp(-hazard*dt)), so no numerical integration is needed.
    // hazard is recomputed from current inputs at each checkpoint, so treating it as constant over
    // the short dt and composing the per-interval probabilities reconstructs the time-varying
    // integral. The ln(2) factor makes a reference-size op at mid-pressure a one-second shed
    // half-life; larger ops are shed proportionally faster. See README.
    if (lowMarkPercent < 0 || memLimitBytes <= 0 || rssBytes < 0 ||
        highMarkPercent <= lowMarkPercent || sizeReferenceBytes <= 0 || opTrackedBytes <= 0) {
        return 0.0;
    }

    const double usagePercent = 100.0 * static_cast<double>(rssBytes) / memLimitBytes;
    const double pressure =
        std::clamp((usagePercent - lowMarkPercent) / (highMarkPercent - lowMarkPercent), 0.0, 1.0);
    if (pressure <= 0.0) {
        return 0.0;  // at/below the low mark
    }
    if (pressure >= 1.0) {
        return 1.0;  // at/above the high mark, regardless of size
    }

    const double sizeWeight = static_cast<double>(opTrackedBytes) / sizeReferenceBytes;
    const double dtSeconds = std::max(0.0, durationCount<Milliseconds>(sinceLastCheck) / 1000.0);
    const double odds = pressure / (1.0 - pressure);
    const double hazard = std::numbers::ln2 * sizeWeight * odds;
    return 1.0 - std::exp(-hazard * dtSeconds);
}

Date_t lastEvalTimeForTest(OperationContext* opCtx) {
    return getOpState(opCtx).lastEvalTime;
}
}  // namespace query_memory_load_shedding_detail

bool isOperationQueryMemorySheddingEligible(OperationContext* opCtx) {
    return getOpState(opCtx).eligible;
}

void markOperationQueryMemorySheddingEligible(OperationContext* opCtx) {
    getOpState(opCtx).eligible = true;
}

Status queryMemoryCheckLoadShedding(OperationContext* opCtx) {
    if (!loadSheddingEnabled()) {
        return Status::OK();
    }

    // Only memory-tracking operations are eligible; never create a tracker here.
    auto* tracker = OperationMemoryUsageTracker::getIfExists(opCtx);
    if (!tracker) {
        return Status::OK();
    }

    // Operations opt-in to load shedding via markOperationQueryMemorySheddingEligible.
    LoadSheddingOpState& opState = getOpState(opCtx);
    if (!opState.eligible) {
        return Status::OK();
    }

    // Never shed an operation that has taken a write-intent (IX/X) global lock: it has begun (or is
    // in) a write, and shedding would surface a RetriableError after a partial, non-idempotent
    // write. This is a sticky, one-way signal on the Locker that survives yields.
    if (shard_role_details::getLocker(opCtx)->wasGlobalLockTakenForWrite()) {
        return Status::OK();
    }

    auto& state = getState(opCtx->getServiceContext());
    const int64_t rss = effectiveRssBytes(state);
    const int64_t memLimit = state.memLimitBytes.loadRelaxed();
    const std::int32_t lowPct = gQueryMemoryLoadSheddingLowMarkPercent.loadRelaxed();

    // Steady-state fast path: below the low mark (or no sample yet) nothing is shed, so skip the
    // clock read and probability calculation. Mark the dt baseline invalid (Date_t::max()) to
    // record that we don't have a dt baseline yet. We'll only shed after the second check that
    // passes this conditional.
    if (rss < 0 || memLimit <= 0 || rss <= memLimit * lowPct / 100) {
        opState.lastEvalTime = Date_t::max();
        return Status::OK();
    }

    // An operation with no tracked memory has zero shed probability and is never a shedding target
    // (e.g. a cursor that has already spilled its memory). Return early so the failpoint path below
    // respects it too, not just the probabilistic path's p <= 0 check.
    const int64_t opTrackedBytes = tracker->inUseTrackedMemoryBytes();
    if (opTrackedBytes <= 0) {
        return Status::OK();
    }

    const Date_t now = opCtx->getServiceContext()->getFastClockSource()->now();

    // The operation is now eligible (enabled, memory-tracked, non-exempt, over the low mark, with
    // tracked memory). The test-only failpoint forces a deterministic shed here, bypassing the
    // dt-based priming/throttle gates and the probabilistic roll below so tests don't depend on
    // evaluation timing.
    if (!MONGO_unlikely(queryMemoryLoadSheddingAlwaysShed.shouldFail())) {
        // First check after crossing above the low mark (opState.lastEvalTime was left invalid by
        // the fast path): do not shed, just reset the clock. Exit early to avoid a negative dt.
        if (opState.lastEvalTime == Date_t::max()) {
            opState.lastEvalTime = now;
            return Status::OK();
        }

        // Throttle: don't update opState.lastEvalTime, so the time accrues.
        Milliseconds dt = now - opState.lastEvalTime;
        if (dt < kMinShedEvalInterval) {
            return Status::OK();
        }
        // Cap dt so a long gap between evaluations can't push the per-check probability toward 1 in
        // a single step. This matters most when the whole system stalls (e.g. a lock convoy or slow
        // storage): otherwise every eligible op resumes with a huge dt at once and is shed
        // near-certainly -- a thundering herd of kills exactly when the server is already
        // struggling. Under-shedding here is safe: the op is re-evaluated at its next checkpoint.
        dt = std::min(dt, Milliseconds(1000));
        opState.lastEvalTime = now;

        const double p = query_memory_load_shedding_detail::shedProbability(
            rss,
            memLimit,
            opTrackedBytes,
            lowPct,
            gQueryMemoryLoadSheddingHighMarkPercent.loadRelaxed(),
            gQueryMemoryLoadSheddingSizeReferenceBytes.loadRelaxed(),
            dt);
        if (p <= 0.0) {
            return Status::OK();
        }

        // A cheap integer-draw roll on the client's warm PRNG.
        if (!opCtx->getClient()->getPrng().trueWithProbability(p)) {
            return Status::OK();
        }
    }

    // Make the shed sticky via markKilled (like the deadline/killOp paths) so it is one-shot and
    // every later interrupt check agrees, not a transient status a caller could swallow.
    opCtx->markKilled(ErrorCodes::QueryMemoryLimitExceeded);

    const long long total = state.operationsShed->fetchAndAdd(1) + 1;
    operationsShedCounter().add(1);
    // Rate-limit to one full line per second to avoid flooding the log under sustained pressure;
    // subsequent lines within the second drop to a debug severity.
    static logv2::SeveritySuppressor logSeverity{
        Seconds{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(2)};
    const auto* curOp = CurOp::get(opCtx);
    LOGV2_DEBUG(13033300,
                logSeverity().toInt(),
                "Load-shedding a memory-tracked operation: process memory is under pressure",
                "opId"_attr = opCtx->getOpID(),
                "ns"_attr = toStringForLogging(curOp->getNSS()),
                "command"_attr = redact(curOp->opDescription()),
                "planSummary"_attr = curOp->getPlanSummary(),
                "currentUsageBytes"_attr = rss,
                "opTrackedBytes"_attr = tracker->inUseTrackedMemoryBytes(),
                "operationsShed"_attr = total);
    return {
        ErrorCodes::QueryMemoryLimitExceeded,
        "Operation load-shed because the server is under memory pressure. Retry when the server "
        "is less loaded."};
}

void appendQueryMemoryLoadSheddingStats(ServiceContext* serviceContext, BSONObjBuilder& b) {
    if (!loadSheddingSupportedOnThisProcess() || !loadSheddingEnabled()) {
        return;
    }
    auto& state = getState(serviceContext);
    const int64_t memLimit = state.memLimitBytes.loadRelaxed();
    const int32_t lowPct = gQueryMemoryLoadSheddingLowMarkPercent.loadRelaxed();
    const int32_t highPct = gQueryMemoryLoadSheddingHighMarkPercent.loadRelaxed();
    const int64_t rss = state.rssBytes.loadRelaxed();
    BSONObjBuilder sub(b.subobjStart("loadShedding"));
    sub.appendNumber("currentUsageBytes", static_cast<long long>(rss < 0 ? 0 : rss));
    sub.appendNumber("lowMarkBytes", static_cast<long long>(memLimit * lowPct / 100));
    sub.appendNumber("highMarkBytes", static_cast<long long>(memLimit * highPct / 100));
    sub.appendNumber("memLimitBytes", static_cast<long long>(memLimit));
    sub.appendNumber("operationsShed", state.operationsShed->loadRelaxed());
}

Status onUpdateQueryMemoryLoadSheddingEnablement(std::int32_t newPercent) {
    // Disabled entirely on a pure router; leave the monitor uninstalled regardless of the knob.
    if (!loadSheddingSupportedOnThisProcess()) {
        return Status::OK();
    }
    auto client = Client::getCurrent();
    if (!client) {
        return Status::OK();
    }
    auto* serviceContext = client->getServiceContext();
    // Start the monitor on runtime enable, or tear it down on runtime disable. Both are no-ops if
    // the state already matches.
    if (newPercent >= 0) {
        // This hook can fire from startup-config application before the PeriodicRunner exists;
        // tolerate that (the explicit mongod startup call starts the monitor once it is installed).
        startQueryMemoryRssMonitor(serviceContext, /*ignoreWhenNoPeriodicRunner*/ true);
    } else {
        stopQueryMemoryRssMonitor(serviceContext);
    }
    return Status::OK();
}

void startQueryMemoryRssMonitor(ServiceContext* serviceContext, bool ignoreWhenNoPeriodicRunner) {
    // Never run on a pure router (see loadSheddingSupportedOnThisProcess): the monitor stays
    // uninstalled there, so shedding cannot fire regardless of the knob.
    if (!loadSheddingSupportedOnThisProcess()) {
        return;
    }
    // Only run the sampler while enabled, so no thread exists when the feature is off (the
    // default).
    if (!loadSheddingEnabled()) {
        return;
    }
    auto& holder = getMonitorHolder(serviceContext);
    std::lock_guard lk(holder.mutex);
    if (holder.anchor) {
        return;
    }
    auto runner = serviceContext->getPeriodicRunner();
    if (!runner) {
        // A missing runner while enabled is a startup-ordering bug (see
        // startQueryMemoryRssMonitor's header).
        tassert(13033301,
                "query-memory RSS monitor requested with load shedding enabled but no "
                "PeriodicRunner is available",
                ignoreWhenNoPeriodicRunner);
        return;
    }
    // Publish a fresh sample immediately so a (re)enable does not evaluate against a stale or
    // absent signal before the first periodic tick.
    publishRssSample(serviceContext);
    // Define and start the monitor job.
    PeriodicRunner::PeriodicJob job(
        "queryMemoryRssMonitor",
        [](Client* client) {
            if (!loadSheddingEnabled()) {
                return;
            }
            publishRssSample(client->getServiceContext());
        },
        Milliseconds(gQueryMemoryRssMonitorIntervalMillis.load()),
        false /*isKillableByStepdown*/);
    holder.anchor = std::make_shared<PeriodicJobAnchor>(runner->makeJob(std::move(job)));
    holder.anchor->start();
}

void stopQueryMemoryRssMonitor(ServiceContext* serviceContext) {
    std::shared_ptr<PeriodicJobAnchor> anchor;
    {
        auto& holder = getMonitorHolder(serviceContext);
        std::lock_guard lk(holder.mutex);
        anchor = std::exchange(holder.anchor, nullptr);
    }
    // stop() blocks until the sampler has quiesced; do it outside the lock so we don't hold the
    // mutex during that wait.
    if (anchor) {
        anchor->stop();
    }
    // Drop the sample so a later re-enable does not shed based on a stale RSS reading.
    getState(serviceContext).rssBytes.store(-1);
}

}  // namespace mongo
