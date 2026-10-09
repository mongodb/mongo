// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/query_memory_load_shedding.h"

#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/memory_tracking/query_memory_load_shedding_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_lifespan.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
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
const auto getProcessState = ServiceContext::declareDecoration<QueryMemoryLoadSheddingState>();

// Per-operation load-shedding bookkeeping. Scoped to the query's QueryLifespan (not the
// OperationContext) so it is set once on the originating command and survives that query's getMores
// and idle periods.
struct LoadSheddingOpState {
    // Opt-in eligibility (see is/markOperationQueryMemorySheddingEligible). Defaults false.
    // Atomic because the queued-operation sweep reads it from the monitor thread. Relaxed access is
    // sufficient because the flag is one-way, so a stale read only delays shedding candidacy.
    // Writes are protected separately by the write-intent lock check in isShedCandidate(), not by
    // this flag.
    bool isEligible() const {
        return _eligible.loadRelaxed();
    }
    void markEligible() {
        _eligible.storeRelaxed(true);
    }

    // Wall-clock time of the previous shed evaluation, used to compute dt. Initialized by the
    // first shedding check. Atomic due to r/w access on both the op's interrupt checks and the
    // queued-operation sweep. Relaxed access is sufficient -- timing inaccuracies just skew the
    // shed probability.
    Date_t lastEvalTime() const {
        return Date_t::fromMillisSinceEpoch(_lastEvalTimeMillis.loadRelaxed());
    }
    void setLastEvalTime(Date_t t) {
        _lastEvalTimeMillis.storeRelaxed(t.toMillisSinceEpoch());
    }

private:
    AtomicWord<bool> _eligible{false};
    AtomicWord<long long> _lastEvalTimeMillis{0};
};
const auto getOpState = QueryLifespan::declareOpCtxDecoration<LoadSheddingOpState>();

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitQueryMemoryLoadShedding", [](ServiceContext* ctx) {
        getProcessState(ctx).memLimitBytes.store(
            static_cast<int64_t>(ProcessInfo::getMemSizeBytes()));
    }};

// Minimum spacing between shed evaluations for one operation, to bound cost on the hot interrupt
// path. Skipped time accrues into the next dt, preserving the time-based probability.
constexpr Milliseconds kMinShedEvalInterval{1};

// Caps the dt charged in one shedding evaluation. This avoids a thundering herd of sheds if the
// whole system stalls. Instead we temporarily under-shed after a stall. Setting
// 'queryMemoryRssMonitorIntervalMillis' above this cap is not recommended because it will
// consistently under-shed queued operations.
constexpr Milliseconds kMaxShedEvalDt{1000};

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
int64_t effectiveRssBytes(const QueryMemoryLoadSheddingState& processState) {
    if (auto sfp = queryMemoryPressureOverride.scoped(); MONGO_unlikely(sfp.isActive())) {
        const long long pct = sfp.getData()["usagePercent"].safeNumberLong();
        return processState.memLimitBytes.loadRelaxed() * pct / 100;
    }
    return processState.rssBytes.loadRelaxed();
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

// The pressure inputs for one shed evaluation, read together so a single decision is
// self-consistent.
struct PressureSnapshot {
    int64_t rssBytes = -1;
    int64_t memLimitBytes = 0;
    int32_t lowMarkPercent = -1;

    // False when there is no signal yet or RSS is at/below the low mark, i.e. nothing is shed.
    bool aboveLowMark() const {
        return rssBytes >= 0 && memLimitBytes > 0 &&
            rssBytes > memLimitBytes * lowMarkPercent / 100;
    }
};

PressureSnapshot samplePressure(const QueryMemoryLoadSheddingState& processState) {
    return {effectiveRssBytes(processState),
            processState.memLimitBytes.loadRelaxed(),
            gQueryMemoryLoadSheddingLowMarkPercent.loadRelaxed()};
}

// The probabilistic shedding decision -- dt bookkeeping plus the PRNG draw. Assumes the caller
// has established that shedding is enabled, that 'pressure' is above the low mark, and that
// 'opTrackedBytes' is positive.
bool rollForShed(const PressureSnapshot& pressure,
                 LoadSheddingOpState& opState,
                 int64_t opTrackedBytes,
                 Date_t now,
                 PseudoRandom& prng) {
    // Test-only: shed deterministically, bypassing the dt gates below so tests don't depend on
    // evaluation timing.
    if (MONGO_unlikely(queryMemoryLoadSheddingAlwaysShed.shouldFail())) {
        return true;
    }

    // No dt baseline yet (see LoadSheddingOpState::lastEvalTime): don't shed, just start the clock.
    const Date_t lastEvalTime = opState.lastEvalTime();
    if (lastEvalTime == Date_t{}) {
        opState.setLastEvalTime(now);
        return false;
    }

    // Throttle: don't update the clock, so the time accrues into the next dt.
    Milliseconds dt = now - lastEvalTime;
    if (dt < kMinShedEvalInterval) {
        return false;
    }
    dt = std::min(dt, kMaxShedEvalDt);
    opState.setLastEvalTime(now);

    const double p = query_memory_load_shedding_detail::shedProbability(
        pressure.rssBytes,
        pressure.memLimitBytes,
        opTrackedBytes,
        pressure.lowMarkPercent,
        gQueryMemoryLoadSheddingHighMarkPercent.loadRelaxed(),
        gQueryMemoryLoadSheddingSizeReferenceBytes.loadRelaxed(),
        dt);
    if (p <= 0.0) {
        return false;
    }

    // A cheap integer-draw roll on the caller's warm PRNG.
    return prng.trueWithProbability(p);
}

bool isShedCandidate(OperationContext* opCtx,
                     const LoadSheddingOpState& opState,
                     int64_t opTrackedBytes) {
    // Operations opt in to load shedding via markOperationQueryMemorySheddingEligible.
    if (!opState.isEligible()) {
        return false;
    }
    // An operation with no tracked memory has zero shed probability and is never a shedding target
    // (e.g. a cursor that has already spilled its memory). Excluding it here also keeps the
    // always-shed test failpoint from shedding it.
    if (opTrackedBytes <= 0) {
        return false;
    }
    // Never shed an operation that has taken a write-intent (IX/X) global lock: it has begun (or is
    // in) a write, and shedding would surface a RetriableError after a partial, non-idempotent
    // write. This is a sticky, one-way signal on the Locker that survives yields, and is atomic so
    // the queued-operation sweep can read it from another thread. It also latches on a global S
    // lock, which only fsyncLock takes, so it excludes no shed-eligible read.
    return !shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites();
}

// Bookkeeping and logging for a shed.
void recordShed(QueryMemoryLoadSheddingState& processState,
                OperationContext* opCtx,
                const PressureSnapshot& pressure,
                int64_t opTrackedBytes,
                bool waitingForTicket) {
    const long long total = processState.operationsShed->fetchAndAdd(1) + 1;
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
                "waitingForTicket"_attr = waitingForTicket,
                "currentUsageBytes"_attr = pressure.rssBytes,
                "opTrackedBytes"_attr = opTrackedBytes,
                "operationsShed"_attr = total);
}

// Samples the process RSS once and publishes it as the current signal.
void publishRssSample(ServiceContext* serviceContext) {
    const int64_t rssBytes = static_cast<int64_t>(ProcessInfo{}.getResidentSize()) * 1024 * 1024;
    getProcessState(serviceContext).rssBytes.store(rssBytes);
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
    return getOpState(opCtx).lastEvalTime();
}
}  // namespace query_memory_load_shedding_detail

bool isOperationQueryMemorySheddingEligible(OperationContext* opCtx) {
    return getOpState(opCtx).isEligible();
}

void markOperationQueryMemorySheddingEligible(OperationContext* opCtx) {
    getOpState(opCtx).markEligible();
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

    LoadSheddingOpState& opState = getOpState(opCtx);
    auto& processState = getProcessState(opCtx->getServiceContext());
    const PressureSnapshot pressure = samplePressure(processState);

    // Steady-state fast path: below the low mark (or no sample yet) nothing is shed, so skip the
    // eligibility checks, clock read and probability calculation. Mark the dt baseline invalid
    // (Date_t{}) to record that we don't have a dt baseline yet. We'll only shed after the second
    // check that passes this conditional.
    if (!pressure.aboveLowMark()) {
        opState.setLastEvalTime(Date_t{});
        return Status::OK();
    }

    const int64_t opTrackedBytes = tracker->inUseTrackedMemoryBytes();
    if (!isShedCandidate(opCtx, opState, opTrackedBytes)) {
        return Status::OK();
    }

    // The operation is now eligible (enabled, memory-tracked, non-exempt, over the low mark, with
    // tracked memory), so try probabilistic shedding.
    const Date_t now = opCtx->getServiceContext()->getFastClockSource()->now();
    if (!rollForShed(pressure, opState, opTrackedBytes, now, opCtx->getClient()->getPrng())) {
        return Status::OK();
    }

    // Make the shed sticky via markKilled (like the deadline/killOp paths) so it is one-shot and
    // every later interrupt check agrees, not a transient status a caller could swallow.
    opCtx->markKilled(ErrorCodes::QueryMemoryLimitExceeded);
    recordShed(processState, opCtx, pressure, opTrackedBytes, /*waitingForTicket*/ false);
    return {
        ErrorCodes::QueryMemoryLimitExceeded,
        "Operation load-shed because the server is under memory pressure. Retry when the server "
        "is less loaded."};
}

namespace {

// True when 'opCtx' is blocked waiting for a read ticket (or a shared lock) and so cannot reach any
// query-execution checkpoint on its own. Queued writers are excluded. We accept a minor race: the
// op can be granted its ticket, take a write lock and begin writing between this check and
// killOperation(), and then be killed with a retryable error after a partial write.
// TODO SERVER-136543: shed queued operations on their own thread while they wait for a ticket, and
// remove this sweep.
bool isQueuedReader(OperationContext* opCtx) {
    const Locker* locker = shard_role_details::getLocker(opCtx);
    if (!locker) {
        return false;
    }
    return locker->getClientState() == Locker::ClientState::kQueuedReader;
}

}  // namespace

// Runs on the monitor thread, so it can't use queryMemoryCheckLoadShedding(), which must run on the
// op's own thread. It reads the op's tracked bytes from CurOp's atomic copy instead of the tracker,
// rolls on the caller's PRNG instead of the op's Client PRNG, and sheds via killOperation() under
// the ClientLock so the op's blocked ticket/lock wait is interrupted.
size_t sweepQueuedOperationsForMemoryPressure(ServiceContext* serviceContext, PseudoRandom& prng) {
    size_t candidates = 0;
    if (!loadSheddingSupportedOnThisProcess() || !loadSheddingEnabled()) {
        return candidates;
    }
    auto& processState = getProcessState(serviceContext);
    const PressureSnapshot pressure = samplePressure(processState);
    if (!pressure.aboveLowMark()) {
        return candidates;
    }
    // Skip the client walk when no reader is queued. This is the same state isQueuedReader()
    // selects, read from Locker's lock-free global counts.
    if (Locker::getGlobalClientStateCounts().queuedReader <= 0) {
        return candidates;
    }

    const Date_t now = serviceContext->getFastClockSource()->now();
    for (ServiceContext::LockedClientsCursor cursor(serviceContext);
         Client* client = cursor.next();) {
        ClientLock lk(client);
        OperationContext* opCtx = client->getOperationContext();
        if (!opCtx || opCtx->isKillPending() || opCtx->isKillOpsExempt()) {
            continue;
        }
        // Evaluate only operations that are not executing.
        if (!isQueuedReader(opCtx)) {
            continue;
        }
        // Eligibility state lives on the QueryLifespan; never create one from another thread.
        if (!QueryLifespan::getIfExists(opCtx)) {
            continue;
        }
        LoadSheddingOpState& opState = getOpState(opCtx);
        // Tracked bytes as last published to CurOp (atomic; updated by the op's own thread each
        // time its trackers cross a reporting chunk boundary).
        const int64_t opTrackedBytes = CurOp::get(opCtx)->getInUseTrackedMemoryBytes();
        if (!isShedCandidate(opCtx, opState, opTrackedBytes)) {
            continue;
        }
        ++candidates;
        // The op is eligible, so give it a chance at probabilistic shedding.
        if (!rollForShed(pressure, opState, opTrackedBytes, now, prng)) {
            continue;
        }
        // The random roll was positive, so shed the query.
        serviceContext->killOperation(lk, opCtx, ErrorCodes::QueryMemoryLimitExceeded);
        recordShed(processState, opCtx, pressure, opTrackedBytes, /*waitingForTicket*/ true);
    }
    return candidates;
}

void appendQueryMemoryLoadSheddingStats(ServiceContext* serviceContext, BSONObjBuilder& b) {
    if (!loadSheddingSupportedOnThisProcess() || !loadSheddingEnabled()) {
        return;
    }
    auto& processState = getProcessState(serviceContext);
    const int64_t memLimit = processState.memLimitBytes.loadRelaxed();
    const int32_t lowPct = gQueryMemoryLoadSheddingLowMarkPercent.loadRelaxed();
    const int32_t highPct = gQueryMemoryLoadSheddingHighMarkPercent.loadRelaxed();
    const int64_t rss = processState.rssBytes.loadRelaxed();
    BSONObjBuilder sub(b.subobjStart("loadShedding"));
    sub.appendNumber("currentUsageBytes", static_cast<long long>(rss < 0 ? 0 : rss));
    sub.appendNumber("lowMarkBytes", static_cast<long long>(memLimit * lowPct / 100));
    sub.appendNumber("highMarkBytes", static_cast<long long>(memLimit * highPct / 100));
    sub.appendNumber("memLimitBytes", static_cast<long long>(memLimit));
    sub.appendNumber("operationsShed", processState.operationsShed->loadRelaxed());
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
        startQueryMemoryRssMonitor(serviceContext, /*ignoreWhenNoPeriodicRunner=*/true);
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
            sweepQueuedOperationsForMemoryPressure(client->getServiceContext(), client->getPrng());
        },
        Milliseconds(gQueryMemoryRssMonitorIntervalMillis.load()),
        /*isKillableByStepdown=*/false);
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
    getProcessState(serviceContext).rssBytes.store(-1);
}

}  // namespace mongo
