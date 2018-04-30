/**
 *    Copyright (C) 2008, 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/clientcursor.h"

#include <string>
#include <time.h>
#include <vector>

#include "mongo/base/counter.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/cursor_server_params.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"

namespace mongo {

using std::string;
using std::stringstream;

static Counter64 cursorStatsOpen;           // gauge
static Counter64 cursorStatsOpenPinned;     // gauge
static Counter64 cursorStatsOpenNoTimeout;  // gauge
static Counter64 cursorStatsTimedOut;

static ServerStatusMetricField<Counter64> dCursorStatsOpen("cursor.open.total", &cursorStatsOpen);
static ServerStatusMetricField<Counter64> dCursorStatsOpenPinned("cursor.open.pinned",
                                                                 &cursorStatsOpenPinned);
static ServerStatusMetricField<Counter64> dCursorStatsOpenNoTimeout("cursor.open.noTimeout",
                                                                    &cursorStatsOpenNoTimeout);
static ServerStatusMetricField<Counter64> dCursorStatusTimedout("cursor.timedOut",
                                                                &cursorStatsTimedOut);

long long ClientCursor::totalOpen() {
    return cursorStatsOpen.get();
}

ClientCursor::ClientCursor(ClientCursorParams params,
                           CursorManager* cursorManager,
                           CursorId cursorId,
                           boost::optional<LogicalSessionId> lsid,
                           Date_t now)
    : _cursorid(cursorId),
      _nss(std::move(params.nss)),
      _authenticatedUsers(std::move(params.authenticatedUsers)),
      _lsid(std::move(lsid)),
      _isReadCommitted(params.isReadCommitted),
      _cursorManager(cursorManager),
      _originatingCommand(params.originatingCommandObj),
      _queryOptions(params.queryOptions),
      _exec(std::move(params.exec)),
      _lastUseDate(now) {
    invariant(_cursorManager);
    invariant(_exec);

    cursorStatsOpen.increment();

    if (isNoTimeout()) {
        // cursors normally timeout after an inactivity period to prevent excess memory use
        // setting this prevents timeout of the cursor in question.
        cursorStatsOpenNoTimeout.increment();
    }
}

ClientCursor::~ClientCursor() {
    // Cursors must be unpinned and deregistered from their cursor manager before being deleted.
    invariant(!_isPinned);
    invariant(_disposed);

    cursorStatsOpen.decrement();
    if (isNoTimeout()) {
        cursorStatsOpenNoTimeout.decrement();
    }
}

void ClientCursor::markAsKilled(const std::string& reason) {
    _exec->markAsKilled(reason);
}

void ClientCursor::dispose(OperationContext* opCtx) {
    if (_disposed) {
        return;
    }

    _exec->dispose(opCtx, _cursorManager);
    _disposed = true;
}

void ClientCursor::updateSlaveLocation(OperationContext* opCtx) {
    if (_slaveReadTill.isNull())
        return;

    verify(_nss.isOplog());

    Client* c = opCtx->getClient();
    verify(c);
    OID rid = repl::ReplClientInfo::forClient(c).getRemoteID();
    if (!rid.isSet())
        return;

    repl::getGlobalReplicationCoordinator()
        ->setLastOptimeForSlave(rid, _slaveReadTill)
        .transitional_ignore();
}

//
// Pin methods
//

ClientCursorPin::ClientCursorPin(OperationContext* opCtx, ClientCursor* cursor)
    : _opCtx(opCtx), _cursor(cursor) {
    invariant(_cursor);
    invariant(_cursor->_isPinned);
    invariant(_cursor->_cursorManager);
    invariant(!_cursor->_disposed);

    // We keep track of the number of cursors currently pinned. The cursor can become unpinned
    // either by being released back to the cursor manager or by being deleted. A cursor may be
    // transferred to another pin object via move construction or move assignment, but in this case
    // it is still considered pinned.
    cursorStatsOpenPinned.increment();
}

ClientCursorPin::ClientCursorPin(ClientCursorPin&& other)
    : _opCtx(other._opCtx), _cursor(other._cursor) {
    // The pinned cursor is being transferred to us from another pin. The 'other' pin must have a
    // pinned cursor.
    invariant(other._cursor);
    invariant(other._cursor->_isPinned);

    // Be sure to set the 'other' pin's cursor to null in order to transfer ownership to ourself.
    other._cursor = nullptr;
    other._opCtx = nullptr;
}

ClientCursorPin& ClientCursorPin::operator=(ClientCursorPin&& other) {
    if (this == &other) {
        return *this;
    }

    // The pinned cursor is being transferred to us from another pin. The 'other' pin must have a
    // pinned cursor, and we must not have a cursor.
    invariant(!_cursor);
    invariant(other._cursor);
    invariant(other._cursor->_isPinned);

    // Copy the cursor pointer to ourselves, but also be sure to set the 'other' pin's cursor to
    // null so that it no longer has the cursor pinned.
    // Be sure to set the 'other' pin's cursor to null in order to transfer ownership to ourself.
    _cursor = other._cursor;
    other._cursor = nullptr;

    _opCtx = other._opCtx;
    other._opCtx = nullptr;

    return *this;
}

ClientCursorPin::~ClientCursorPin() {
    release();
}

void ClientCursorPin::release() {
    if (!_cursor)
        return;

    // Note it's not safe to dereference _cursor->_cursorManager unless we know we haven't been
    // killed. If we're not locked we assume we haven't been killed because we're working with the
    // global cursor manager which never kills cursors.
    const bool isLocked =
        _opCtx->lockState()->isCollectionLockedForMode(_cursor->_nss.ns(), MODE_IS);
    dassert(isLocked || _cursor->_cursorManager->isGlobalManager());

    invariant(_cursor->_isPinned);

    if (_cursor->getExecutor()->isMarkedAsKilled()) {
        // The ClientCursor was killed while we had it.  Therefore, it is our responsibility to
        // call dispose() and delete it.
        deleteUnderlying();
    } else {
        // Unpin the cursor under the collection cursor manager lock.
        _cursor->_cursorManager->unpin(
            _opCtx, std::unique_ptr<ClientCursor, ClientCursor::Deleter>(_cursor));
        cursorStatsOpenPinned.decrement();
    }

    _cursor = nullptr;
}

void ClientCursorPin::deleteUnderlying() {
    invariant(_cursor);
    invariant(_cursor->_isPinned);
    // Note the following subtleties of this method's implementation:
    // - We must unpin the cursor before destruction, since it is an error to delete a pinned
    //   cursor.
    // - In addition, we must deregister the cursor before unpinning, since it is an
    //   error to unpin a registered cursor without holding the cursor manager lock (note that
    //   we can't simply unpin with the cursor manager lock here, since we need to guarantee
    //   exclusive ownership of the cursor when we are deleting it).

    // Note it's not safe to dereference _cursor->_cursorManager unless we know we haven't been
    // killed. If we're not locked we assume we haven't been killed because we're working with the
    // global cursor manager which never kills cursors.
    const bool isLocked =
        _opCtx->lockState()->isCollectionLockedForMode(_cursor->_nss.ns(), MODE_IS);
    dassert(isLocked || _cursor->_cursorManager->isGlobalManager());

    if (!_cursor->getExecutor()->isMarkedAsKilled()) {
        _cursor->_cursorManager->deregisterCursor(_cursor);
    }

    // Make sure the cursor is disposed and unpinned before being destroyed.
    _cursor->dispose(_opCtx);
    _cursor->_isPinned = false;
    delete _cursor;

    cursorStatsOpenPinned.decrement();
    _cursor = nullptr;
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
    std::string name() const {
        return "ClientCursorMonitor";
    }

    void run() {
        Client::initThread("clientcursormon");
        while (!globalInShutdownDeprecated()) {
            {
                const ServiceContext::UniqueOperationContext opCtx = cc().makeOperationContext();
                auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();
                cursorStatsTimedOut.increment(
                    CursorManager::timeoutCursorsGlobal(opCtx.get(), now));
            }
            MONGO_IDLE_THREAD_BLOCK;
            sleepsecs(getClientCursorMonitorFrequencySecs());
        }
    }
};

// Only one instance of the ClientCursorMonitor exists
ClientCursorMonitor clientCursorMonitor;

void _appendCursorStats(BSONObjBuilder& b) {
    b.append("note", "deprecated, use server status metrics");
    b.appendNumber("clientCursors_size", cursorStatsOpen.get());
    b.appendNumber("totalOpen", cursorStatsOpen.get());
    b.appendNumber("pinned", cursorStatsOpenPinned.get());
    b.appendNumber("totalNoTimeout", cursorStatsOpenNoTimeout.get());
    b.appendNumber("timedOut", cursorStatsTimedOut.get());
}
}

void startClientCursorMonitor() {
    clientCursorMonitor.go();
}

}  // namespace mongo
