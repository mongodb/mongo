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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/external_data_source_scope_guard.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/cursor_server_params.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_knob_configuration.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/background.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"

#include <iosfwd>
#include <mutex>
#include <ratio>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

auto& gCursorStats = *new CursorStats{};
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
      _queryStatsKeyHash(CurOp::get(operationUsingCursor)->debug().queryStatsInfo.keyHash),
      _queryStatsKey(std::move(CurOp::get(operationUsingCursor)->debug().queryStatsInfo.key)),
      _queryStatsWillNeverExhaust(
          CurOp::get(operationUsingCursor)->debug().queryStatsInfo.willNeverExhaust),
      _isChangeStreamQuery(CurOp::get(operationUsingCursor)->debug().isChangeStreamQuery),
      _shouldOmitDiagnosticInformation(
          CurOp::get(operationUsingCursor)->getShouldOmitDiagnosticInformation()),
      _opKey(operationUsingCursor->getOperationKey()) {
    invariant(_exec);
    invariant(_operationUsingCursor);

    cursorStats().open.increment();
    cursorStats().totalOpened.increment();

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
    }

    cursorStats().open.decrement();
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

    // Unpin the cursor. This must be done by calling into the cursor manager, since the cursor
    // manager must acquire the appropriate mutex in order to safely perform the unpin operation.
    _cursorManager->unpin(_opCtx, std::unique_ptr<ClientCursor, ClientCursor::Deleter>(_cursor));
    cursorStats().openPinned.decrement();

    _cursor = nullptr;
}

void ClientCursorPin::deleteUnderlying() {
    invariant(_cursor);
    invariant(_cursor->_operationUsingCursor);
    invariant(_cursorManager);

    std::unique_ptr<ClientCursor, ClientCursor::Deleter> ownedCursor(_cursor);
    _cursor = nullptr;
    _cursorManager->deregisterAndDestroyCursor(_opCtx, std::move(ownedCursor));

    cursorStats().openPinned.decrement();
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
        ThreadClient tc("clientcursormon",
                        getGlobalServiceContext()->getService(ClusterRole::ShardServer));

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
