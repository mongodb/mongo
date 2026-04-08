/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/client_cursor/clientcursor.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/cursor_server_params.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/shard_role/shard_catalog/external_data_source_scope_guard.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/background.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"

#include <string>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

auto& gCursorStats = *new CursorStats{};

namespace ChangeStreamMetrics {

// The total number of change stream cursors opened since the start of the process. This counter
// corresponds to the OTEL metric "change_streams.cursor.total_opened".
const otel::metrics::CounterOptions kCursorsTotalOpenedOpts = [] {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.totalOpened",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();
auto& gCursorsTotalOpened = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kChangeStreamCursorsTotalOpened,
    "Total number of change stream cursors opened (on router or shard).",
    otel::metrics::MetricUnit::kCursors,
    kCursorsTotalOpenedOpts);

// The change stream lifespan histogram is updated after a change stream cursor is closed. A
// histogram provides accurate and thread-safe average for every bucket. This is achieved by locks,
// so there might be some overhead.
const otel::metrics::HistogramOptions kLifespanOpts = [] {
    otel::metrics::HistogramOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.lifespan",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    // Using the same histogram buckets as 'metrics.cursor.lifespan'. For change stream cursors we
    // expect that most of the cursors will land in (10min, +inf) bucket.
    opts.explicitBucketBoundaries = std::vector<double>({
        1 * 1e6,        // lifespan <= 1 second will probably capture one-fetch or no-fetch cursors,
                        // unless the query is slow for some reason
        10 * 1e6,       // lifespan <= 10 seconds will probably capture other 'short-lived' change
                        // stream cursors
        10 * 60 * 1e6,  // lifespan <= 10 minutes will probably capture not 'short-lived', but
                        // before the default cursor timeout
        20 * 60 * 1e6,  // lifespan <= 20 minutes will probably capture increased probability for
                        // cursor timeouts
        60 * 60 * 1e6,  // lifespan <= 1 hour will probably capture some hourly patterns
        24 * 60 * 60 * 1e6,     // lifetime <= 1 day will probably capture some daily patterns
        7 * 24 * 60 * 60 * 1e6  // lifetime <= 1 week will probably capture some weekly patterns
    });
    return opts;
}();
auto& gLifespan = otel::metrics::MetricsService::instance().createInt64Histogram(
    otel::metrics::MetricNames::kChangeStreamCursorsLifespan,
    "Lifespan of closed change stream cursors in microseconds.",
    otel::metrics::MetricUnit::kMicroseconds,
    kLifespanOpts);

// The number of currently open change stream cursors (idle or pinned). This counter corresponds to
// the OTEL metric "change_streams.cursor.open.total".
const otel::metrics::UpDownCounterOptions kCursorsOpenTotalOpts = [] {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.total",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();
auto& gCursorsOpenTotal = otel::metrics::MetricsService::instance().createInt64UpDownCounter(
    otel::metrics::MetricNames::kChangeStreamCursorsOpenTotal,
    "Current number of open change stream cursors.",
    otel::metrics::MetricUnit::kCursors,
    kCursorsOpenTotalOpts);

// The number of currently pinned (active) change stream cursors. This counter corresponds to the
// OTEL metric "change_streams.cursor.open.pinned".
const otel::metrics::UpDownCounterOptions kCursorsOpenPinnedOpts = [] {
    otel::metrics::UpDownCounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.cursor.open.pinned",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();
auto& gCursorsOpenPinned = otel::metrics::MetricsService::instance().createInt64UpDownCounter(
    otel::metrics::MetricNames::kChangeStreamCursorsOpenPinned,
    "Current number of pinned (active) change stream cursors.",
    otel::metrics::MetricUnit::kCursors,
    kCursorsOpenPinnedOpts);
}  // namespace ChangeStreamMetrics
}  // namespace

Counter64& CursorStats::_makeStat(StringData name) {
    static constexpr auto prefix = "cursor"_sd;
    return *MetricBuilder<Counter64>(fmt::format("{}.{}", prefix, name))
                .setRole(ClusterRole::ShardServer);
}

CursorStats& cursorStats() {
    return gCursorStats;
}

void incrementCursorLifespanMetric(Date_t birth, Date_t death) {
    auto elapsed = death - birth;
    if (elapsed < Seconds(1)) {
        cursorStats().lifespanLessThan1Second.increment();
    } else if (elapsed < Seconds(5)) {
        cursorStats().lifespanLessThan5Seconds.increment();
    } else if (elapsed < Seconds(15)) {
        cursorStats().lifespanLessThan15Seconds.increment();
    } else if (elapsed < Seconds(30)) {
        cursorStats().lifespanLessThan30Seconds.increment();
    } else if (elapsed < Minutes(1)) {
        cursorStats().lifespanLessThan1Minute.increment();
    } else if (elapsed < Minutes(10)) {
        cursorStats().lifespanLessThan10Minutes.increment();
    } else {
        cursorStats().lifespanGreaterThanOrEqual10Minutes.increment();
    }
}

const ClientCursor::Decoration<std::shared_ptr<ExternalDataSourceScopeGuard>>
    ExternalDataSourceScopeGuard::get =
        ClientCursor::declareDecoration<std::shared_ptr<ExternalDataSourceScopeGuard>>();

ClientCursor::ClientCursor(ClientCursorParams params,
                           CursorId cursorId,
                           OperationContext* operationUsingCursor,
                           Date_t now)
    : _cursorid(cursorId),
      _nss(std::move(params.nss)),
      _authenticatedUser(std::move(params.authenticatedUser)),
      _lsid(operationUsingCursor->getLogicalSessionId()),
      // Retryable writes will have a txnNumber we do not want to associate with the cursor. We only
      // want to set this field for transactions.
      _txnNumber(operationUsingCursor->inMultiDocumentTransaction()
                     ? operationUsingCursor->getTxnNumber()
                     : boost::none),
      _apiParameters(std::move(params.apiParameters)),
      _writeConcernOptions(std::move(params.writeConcernOptions)),
      _readConcernArgs(std::move(params.readConcernArgs)),
      _readPreferenceSetting(std::move(params.readPreferenceSetting)),
      _originatingCommand(params.originatingCommandObj),
      _originatingPrivileges(std::move(params.originatingPrivileges)),
      _tailableMode(params.tailableMode),
      _isNoTimeout(params.isNoTimeout),
      _exec(std::move(params.exec)),
      _operationUsingCursor(operationUsingCursor),
      _lastUseDate(now),
      _createdDate(now),
      _planSummary(_exec->getPlanExplainer().getPlanSummary()),
      _planCacheKey(CurOp::get(operationUsingCursor)->debug().planCacheKey),
      _planCacheShapeHash(CurOp::get(operationUsingCursor)->debug().planCacheShapeHash),
      _queryShapeHash(CurOp::get(operationUsingCursor)->debug().getQueryShapeHash()),
      _queryStatsKeyHash(CurOp::get(operationUsingCursor)->debug().getQueryStatsInfo().keyHash),
      _queryStatsKey(std::move(CurOp::get(operationUsingCursor)->debug().getQueryStatsInfo().key)),
      _queryStatsWillNeverExhaust(
          CurOp::get(operationUsingCursor)->debug().getQueryStatsInfo().willNeverExhaust),
      _isChangeStreamQuery(CurOp::get(operationUsingCursor)->debug().isChangeStreamQuery),
      _shouldOmitDiagnosticInformation(
          CurOp::get(operationUsingCursor)->getShouldOmitDiagnosticInformation()),
      _opKey(operationUsingCursor->getOperationKey()) {
    invariant(_exec);
    invariant(_operationUsingCursor);

    cursorStats().open.increment();
    cursorStats().totalOpened.increment();

    if (_isChangeStreamQuery) {
        ChangeStreamMetrics::gCursorsTotalOpened.add(1);
        ChangeStreamMetrics::gCursorsOpenTotal.add(1);
    }

    if (isNoTimeout()) {
        // cursors normally timeout after an inactivity period to prevent excess memory use
        // setting this prevents timeout of the cursor in question.
        cursorStats().openNoTimeout.increment();
    }
}

ClientCursor::~ClientCursor() {
    // Cursors must be unpinned and deregistered from their cursor manager before being deleted.
    invariant(!_operationUsingCursor);
    invariant(_disposed);

    // We manually dispose of the PlanExecutor here to release all acquisitions. This must be
    // deleted before the yielded acquisitions since the execution plan may maintain pointers to the
    // TransactionResources.
    _exec.reset();
    // If we are holding transaction resources we must dispose of them before destroying the object.
    // Not doing so is a programming failure.
    _transactionResources.dispose();
}

void ClientCursor::dispose(OperationContext* opCtx, boost::optional<Date_t> now) {
    if (_disposed) {
        return;
    }

    if (now) {
        incrementCursorLifespanMetric(_createdDate, *now);

        if (_isChangeStreamQuery) {
            const int64_t lifespanUs = (*now - _createdDate).count() * 1000;
            ChangeStreamMetrics::gLifespan.record(lifespanUs);
        }
    }

    cursorStats().open.decrement();
    if (_isChangeStreamQuery) {
        ChangeStreamMetrics::gCursorsOpenTotal.add(-1);
    }
    if (isNoTimeout()) {
        cursorStats().openNoTimeout.decrement();
    }

    if (_metrics.nBatches && *_metrics.nBatches > 1) {
        cursorStats().moreThanOneBatch.increment();
    }

    _exec->dispose(opCtx);
    // Update opCtx of the decorated ExternalDataSourceScopeGuard object so that it can drop virtual
    // collections in the new 'opCtx'.
    ExternalDataSourceScopeGuard::updateOperationContext(this, opCtx);
    _disposed = true;


    query_stats::writeQueryStatsOnCursorDisposeOrKill(opCtx,
                                                      _queryStatsKeyHash,
                                                      std::move(_queryStatsKey),
                                                      _queryStatsWillNeverExhaust,
                                                      _firstResponseExecutionTime,
                                                      _metrics);
}

GenericCursor ClientCursor::toGenericCursor() const {
    GenericCursor gc;
    gc.setCursorId(cursorid());
    gc.setNs(nss());
    gc.setNDocsReturned(_metrics.nreturned.value_or(0));
    gc.setTailable(isTailable());
    gc.setAwaitData(isAwaitData());
    gc.setNoCursorTimeout(isNoTimeout());
    gc.setOriginatingCommand(getOriginatingCommandObj());
    gc.setLsid(getSessionId());
    gc.setTxnNumber(_txnNumber);
    gc.setLastAccessDate(getLastUseDate());
    gc.setCreatedDate(getCreatedDate());
    gc.setNBatchesReturned(getNBatches());
    if (_memoryUsageTracker) {
        if (auto inUseTrackedMemBytes = _memoryUsageTracker->inUseTrackedMemoryBytes()) {
            gc.setInUseTrackedMemBytes(inUseTrackedMemBytes);
        }
        if (auto peakTrackedMemBytes = _memoryUsageTracker->peakTrackedMemoryBytes()) {
            gc.setPeakTrackedMemBytes(peakTrackedMemBytes);
        }
    }
    gc.setPlanSummary(getPlanSummary());
    if (auto opCtx = _operationUsingCursor) {
        gc.setOperationUsingCursorId(opCtx->getOpID());
    }
    gc.setLastKnownCommittedOpTime(_lastKnownCommittedOpTime);
    return gc;
}

//
// Pin methods
//

ClientCursorPin::ClientCursorPin(OperationContext* opCtx,
                                 ClientCursor* cursor,
                                 CursorManager* cursorManager)
    : _opCtx(opCtx),
      _cursor(cursor),
      _cursorManager(cursorManager),
      _interruptibleLockGuard(std::make_unique<InterruptibleLockGuard>(opCtx)) {
    invariant(_cursor);
    invariant(_cursor->_operationUsingCursor);
    invariant(!_cursor->_disposed);
    // We keep track of the number of cursors currently pinned. The cursor can become unpinned
    // either by being released back to the cursor manager or by being deleted. A cursor may be
    // transferred to another pin object via move construction or move assignment, but in this case
    // it is still considered pinned.
    cursorStats().openPinned.increment();
    if (_cursor->_isChangeStreamQuery) {
        ChangeStreamMetrics::gCursorsOpenPinned.add(1);
    }
    OperationMemoryUsageTracker::moveToOpCtxIfAvailable(opCtx,
                                                        std::move(_cursor->_memoryUsageTracker));
}

ClientCursorPin::ClientCursorPin(ClientCursorPin&& other)
    : _opCtx(other._opCtx),
      _cursor(other._cursor),
      _cursorManager(other._cursorManager),
      _interruptibleLockGuard(std::move(other._interruptibleLockGuard)) {
    // The pinned cursor is being transferred to us from another pin. The 'other' pin must have a
    // pinned cursor.
    invariant(other._cursor);
    invariant(other._cursor->_operationUsingCursor);

    // Be sure to set the 'other' pin's cursor to null in order to transfer ownership to ourself.
    other._cursor = nullptr;
    other._opCtx = nullptr;
    other._cursorManager = nullptr;
}

ClientCursorPin& ClientCursorPin::operator=(ClientCursorPin&& other) {
    if (this == &other) {
        return *this;
    }

    // The pinned cursor is being transferred to us from another pin. The 'other' pin must have a
    // pinned cursor, and we must not have a cursor.
    invariant(!_cursor);
    invariant(other._cursor);
    invariant(other._cursor->_operationUsingCursor);

    // Copy the cursor pointer to ourselves, but also be sure to set the 'other' pin's cursor to
    // null so that it no longer has the cursor pinned.
    // Be sure to set the 'other' pin's cursor to null in order to transfer ownership to ourself.
    _cursor = other._cursor;
    other._cursor = nullptr;

    _opCtx = other._opCtx;
    other._opCtx = nullptr;

    _cursorManager = other._cursorManager;
    other._cursorManager = nullptr;

    _interruptibleLockGuard = std::move(other._interruptibleLockGuard);

    return *this;
}

ClientCursorPin::~ClientCursorPin() {
    release();
}

void ClientCursorPin::release() {
    if (!_cursor) {
        return;
    }

    invariant(_cursor->_operationUsingCursor);
    invariant(_cursorManager);

    _cursor->_memoryUsageTracker =
        OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(_cursor->_operationUsingCursor);
    const bool isChangeStream = _cursor->_isChangeStreamQuery;

    // Unpin the cursor. This must be done by calling into the cursor manager, since the cursor
    // manager must acquire the appropriate mutex in order to safely perform the unpin operation.
    _cursorManager->unpin(_opCtx, std::unique_ptr<ClientCursor, ClientCursor::Deleter>(_cursor));
    cursorStats().openPinned.decrement();
    if (isChangeStream) {
        ChangeStreamMetrics::gCursorsOpenPinned.add(-1);
    }

    _cursor = nullptr;
}

void ClientCursorPin::deleteUnderlying() {
    invariant(_cursor);
    invariant(_cursor->_operationUsingCursor);
    invariant(_cursorManager);

    const bool isChangeStreamQuery = _cursor->_isChangeStreamQuery;
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> ownedCursor(_cursor);
    _cursor = nullptr;
    _cursorManager->deregisterAndDestroyCursor(_opCtx, std::move(ownedCursor));

    cursorStats().openPinned.decrement();
    if (isChangeStreamQuery) {
        ChangeStreamMetrics::gCursorsOpenPinned.add(-1);
    }
}

ClientCursor* ClientCursorPin::getCursor() const {
    return _cursor;
}

namespace {
//
// ClientCursorMonitor
//

/**
 * Thread for timing out inactive cursors.
 */
class ClientCursorMonitor : public BackgroundJob {
public:
    std::string name() const override {
        return "ClientCursorMonitor";
    }

    void run() override {
        ThreadClient tc("clientcursormon", getGlobalServiceContext()->getService());

        while (!globalInShutdownDeprecated()) {
            {
                const ServiceContext::UniqueOperationContext opCtx = cc().makeOperationContext();
                auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();
                try {
                    cursorStats().timedOut.increment(
                        CursorManager::get(opCtx.get())->timeoutCursors(opCtx.get(), now));
                } catch (const DBException& e) {
                    LOGV2_WARNING(
                        7466202,
                        "Cursor Time Out job encountered unexpected error, will retry after cursor "
                        "time out interval",
                        "error"_attr = e.toString());
                }
            }
            MONGO_IDLE_THREAD_BLOCK;
            sleepsecs(getClientCursorMonitorFrequencySecs());
        }
    }
};

auto getClientCursorMonitor = ServiceContext::declareDecoration<ClientCursorMonitor>();
}  // namespace

void startClientCursorMonitor() {
    getClientCursorMonitor(getGlobalServiceContext()).go();
}

}  // namespace mongo
