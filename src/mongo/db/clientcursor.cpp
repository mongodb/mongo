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

#include "mongo/db/clientcursor.h"

#include <ctime>
#include <string>
#include <vector>

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/external_data_source_scope_guard.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/cursor_server_params.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/telemetry.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"

namespace mongo {

using std::string;
using std::stringstream;

static CounterMetric cursorStatsOpen{"cursor.open.total"};
static CounterMetric cursorStatsOpenPinned{"cursor.open.pinned"};
static CounterMetric cursorStatsOpenNoTimeout{"cursor.open.noTimeout"};
static CounterMetric cursorStatsTimedOut{"cursor.timedOut"};
static CounterMetric cursorStatsTotalOpened{"cursor.totalOpened"};
static CounterMetric cursorStatsMoreThanOneBatch{"cursor.moreThanOneBatch"};

static CounterMetric cursorStatsLifespanLessThan1Second{"cursor.lifespan.lessThan1Second"};
static CounterMetric cursorStatsLifespanLessThan5Seconds{"cursor.lifespan.lessThan5Seconds"};
static CounterMetric cursorStatsLifespanLessThan15Seconds{"cursor.lifespan.lessThan15Seconds"};
static CounterMetric cursorStatsLifespanLessThan30Seconds{"cursor.lifespan.lessThan30Seconds"};
static CounterMetric cursorStatsLifespanLessThan1Minute{"cursor.lifespan.lessThan1Minute"};
static CounterMetric cursorStatsLifespanLessThan10Minutes{"cursor.lifespan.lessThan10Minutes"};
static CounterMetric cursorStatsLifespanGreaterThanOrEqual10Minutes{
    "cursor.lifespan.greaterThanOrEqual10Minutes"};

void incrementCursorLifespanMetric(Date_t birth, Date_t death) {
    auto elapsed = death - birth;

    if (elapsed < Seconds(1)) {
        cursorStatsLifespanLessThan1Second.increment();
    } else if (elapsed < Seconds(5)) {
        cursorStatsLifespanLessThan5Seconds.increment();
    } else if (elapsed < Seconds(15)) {
        cursorStatsLifespanLessThan15Seconds.increment();
    } else if (elapsed < Seconds(30)) {
        cursorStatsLifespanLessThan30Seconds.increment();
    } else if (elapsed < Minutes(1)) {
        cursorStatsLifespanLessThan1Minute.increment();
    } else if (elapsed < Minutes(10)) {
        cursorStatsLifespanLessThan10Minutes.increment();
    } else {
        cursorStatsLifespanGreaterThanOrEqual10Minutes.increment();
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
      _txnNumber(operationUsingCursor->getTxnNumber()),
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
      _queryHash(CurOp::get(operationUsingCursor)->debug().queryHash),
      _telemetryStoreKey(CurOp::get(operationUsingCursor)->debug().telemetryStoreKey),
      _shouldOmitDiagnosticInformation(
          CurOp::get(operationUsingCursor)->debug().shouldOmitDiagnosticInformation),
      _opKey(operationUsingCursor->getOperationKey()) {
    invariant(_exec);
    invariant(_operationUsingCursor);

    cursorStatsOpen.increment();
    cursorStatsTotalOpened.increment();

    if (isNoTimeout()) {
        // cursors normally timeout after an inactivity period to prevent excess memory use
        // setting this prevents timeout of the cursor in question.
        cursorStatsOpenNoTimeout.increment();
    }
}

ClientCursor::~ClientCursor() {
    // Cursors must be unpinned and deregistered from their cursor manager before being deleted.
    invariant(!_operationUsingCursor);
    invariant(_disposed);

    if (_stashedRecoveryUnit) {
        // Now that the associated PlanExecutor is being destroyed, the recovery unit no longer
        // needs to keep data pinned.
        _stashedRecoveryUnit->setAbandonSnapshotMode(RecoveryUnit::AbandonSnapshotMode::kAbort);
    }
}

void ClientCursor::dispose(OperationContext* opCtx, boost::optional<Date_t> now) {
    if (_disposed) {
        return;
    }

    if (_telemetryStoreKey && opCtx) {
        telemetry::writeTelemetry(opCtx,
                                  _telemetryStoreKey,
                                  getOriginatingCommandObj(),
                                  _metrics.executionTime.value_or(Microseconds{0}).count(),
                                  _metrics.nreturned.value_or(0));
    }

    if (now) {
        incrementCursorLifespanMetric(_createdDate, *now);
    }

    cursorStatsOpen.decrement();
    if (isNoTimeout()) {
        cursorStatsOpenNoTimeout.decrement();
    }

    if (_metrics.nBatches && *_metrics.nBatches > 1) {
        cursorStatsMoreThanOneBatch.increment();
    }

    _exec->dispose(opCtx);
    // Update opCtx of the decorated ExternalDataSourceScopeGuard object so that it can drop virtual
    // collections in the new 'opCtx'.
    ExternalDataSourceScopeGuard::updateOperationContext(this, opCtx);
    _disposed = true;
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
    gc.setLastAccessDate(getLastUseDate());
    gc.setCreatedDate(getCreatedDate());
    gc.setNBatchesReturned(getNBatches());
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
      _interruptibleLockGuard(std::make_unique<InterruptibleLockGuard>(opCtx->lockState())) {
    invariant(_cursor);
    invariant(_cursor->_operationUsingCursor);
    invariant(!_cursor->_disposed);
    _shouldSaveRecoveryUnit = _cursor->getExecutor()->isSaveRecoveryUnitAcrossCommandsEnabled();

    // We keep track of the number of cursors currently pinned. The cursor can become unpinned
    // either by being released back to the cursor manager or by being deleted. A cursor may be
    // transferred to another pin object via move construction or move assignment, but in this case
    // it is still considered pinned.
    cursorStatsOpenPinned.increment();
}

ClientCursorPin::ClientCursorPin(ClientCursorPin&& other)
    : _opCtx(other._opCtx),
      _cursor(other._cursor),
      _cursorManager(other._cursorManager),
      _interruptibleLockGuard(std::move(other._interruptibleLockGuard)),
      _shouldSaveRecoveryUnit(other._shouldSaveRecoveryUnit) {
    // The pinned cursor is being transferred to us from another pin. The 'other' pin must have a
    // pinned cursor.
    invariant(other._cursor);
    invariant(other._cursor->_operationUsingCursor);

    // Be sure to set the 'other' pin's cursor to null in order to transfer ownership to ourself.
    other._cursor = nullptr;
    other._opCtx = nullptr;
    other._cursorManager = nullptr;
    other._shouldSaveRecoveryUnit = false;
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

    _shouldSaveRecoveryUnit = other._shouldSaveRecoveryUnit;
    other._shouldSaveRecoveryUnit = false;

    return *this;
}

ClientCursorPin::~ClientCursorPin() {
    release();
}

void ClientCursorPin::release() {
    if (!_cursor) {
        invariant(!_shouldSaveRecoveryUnit);
        return;
    }

    invariant(_cursor->_operationUsingCursor);
    invariant(_cursorManager);

    if (_shouldSaveRecoveryUnit) {
        stashResourcesFromOperationContext();
        _shouldSaveRecoveryUnit = false;
    }

    // Unpin the cursor. This must be done by calling into the cursor manager, since the cursor
    // manager must acquire the appropriate mutex in order to safely perform the unpin operation.
    _cursorManager->unpin(_opCtx, std::unique_ptr<ClientCursor, ClientCursor::Deleter>(_cursor));
    cursorStatsOpenPinned.decrement();

    _cursor = nullptr;
}

void ClientCursorPin::deleteUnderlying() {
    invariant(_cursor);
    invariant(_cursor->_operationUsingCursor);
    invariant(_cursorManager);

    std::unique_ptr<ClientCursor, ClientCursor::Deleter> ownedCursor(_cursor);
    _cursor = nullptr;
    _cursorManager->deregisterAndDestroyCursor(_opCtx, std::move(ownedCursor));

    cursorStatsOpenPinned.decrement();
    _shouldSaveRecoveryUnit = false;
}

ClientCursor* ClientCursorPin::getCursor() const {
    return _cursor;
}

void ClientCursorPin::unstashResourcesOntoOperationContext() {
    invariant(_cursor);
    invariant(_cursor->_operationUsingCursor);
    invariant(_opCtx == _cursor->_operationUsingCursor);

    if (auto& ru = _cursor->_stashedRecoveryUnit) {
        _shouldSaveRecoveryUnit = true;
        invariant(!_opCtx->recoveryUnit()->isActive());
        _opCtx->setRecoveryUnit(std::move(ru),
                                WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }
}

void ClientCursorPin::stashResourcesFromOperationContext() {
    // Move the recovery unit from the operation context onto the cursor and create a new RU for
    // the current OperationContext.
    _cursor->stashRecoveryUnit(_opCtx->releaseAndReplaceRecoveryUnit());
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
    std::string name() const {
        return "ClientCursorMonitor";
    }

    void run() {
        ThreadClient tc("clientcursormon", getGlobalServiceContext());
        while (!globalInShutdownDeprecated()) {
            {
                const ServiceContext::UniqueOperationContext opCtx = cc().makeOperationContext();
                auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();
                cursorStatsTimedOut.increment(
                    CursorManager::get(opCtx.get())->timeoutCursors(opCtx.get(), now));
            }
            MONGO_IDLE_THREAD_BLOCK;
            sleepsecs(getClientCursorMonitorFrequencySecs());
        }
    }
};

auto getClientCursorMonitor = ServiceContext::declareDecoration<ClientCursorMonitor>();

void _appendCursorStats(BSONObjBuilder& b) {
    b.append("note", "deprecated, use server status metrics");
    b.appendNumber("clientCursors_size", cursorStatsOpen.get());
    b.appendNumber("totalOpen", cursorStatsOpen.get());
    b.appendNumber("pinned", cursorStatsOpenPinned.get());
    b.appendNumber("totalNoTimeout", cursorStatsOpenNoTimeout.get());
    b.appendNumber("timedOut", cursorStatsTimedOut.get());
}
}  // namespace

void startClientCursorMonitor() {
    getClientCursorMonitor(getGlobalServiceContext()).go();
}

void collectTelemetryMongod(OperationContext* opCtx,
                            ClientCursorPin& pinnedCursor,
                            long long nreturned) {
    auto curOp = CurOp::get(opCtx);
    telemetry::collectMetricsOnOpDebug(curOp, nreturned);
    pinnedCursor->incrementCursorMetrics(curOp->debug().additiveMetrics);
}

void collectTelemetryMongod(OperationContext* opCtx,
                            const BSONObj& originatingCommand,
                            long long nreturned) {
    auto curOp = CurOp::get(opCtx);
    telemetry::collectMetricsOnOpDebug(curOp, nreturned);

    // If we haven't registered a cursor to prepare for getMore requests, we record
    // telemetry directly.
    auto& opDebug = curOp->debug();
    telemetry::writeTelemetry(
        opCtx,
        opDebug.telemetryStoreKey,
        originatingCommand,
        opDebug.additiveMetrics.executionTime.value_or(Microseconds{0}).count(),
        opDebug.additiveMetrics.nreturned.value_or(0));
}

}  // namespace mongo
