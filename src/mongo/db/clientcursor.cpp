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
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/background.h"
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

MONGO_EXPORT_SERVER_PARAMETER(cursorTimeoutMillis, int, 10 * 60 * 1000 /* 10 minutes */);
MONGO_EXPORT_SERVER_PARAMETER(clientCursorMonitorFrequencySecs, int, 4);

long long ClientCursor::totalOpen() {
    return cursorStatsOpen.get();
}

ClientCursor::ClientCursor(CursorManager* cursorManager,
                           PlanExecutor* exec,
                           const std::string& ns,
                           bool isReadCommitted,
                           int qopts,
                           const BSONObj query,
                           bool isAggCursor)
    : _ns(ns),
      _isReadCommitted(isReadCommitted),
      _cursorManager(cursorManager),
      _countedYet(false),
      _isAggCursor(isAggCursor) {
    _exec.reset(exec);
    _query = query;
    _queryOptions = qopts;
    init();
}

ClientCursor::ClientCursor(const Collection* collection)
    : _ns(collection->ns().ns()),
      _isReadCommitted(false),
      _cursorManager(collection->getCursorManager()),
      _countedYet(false),
      _queryOptions(QueryOption_NoCursorTimeout),
      _isAggCursor(false) {
    init();
}

void ClientCursor::init() {
    invariant(_cursorManager);

    _isPinned = false;
    _isNoTimeout = false;

    _idleAgeMillis = 0;
    _leftoverMaxTimeMicros = Microseconds::max();
    _pos = 0;

    if (_queryOptions & QueryOption_NoCursorTimeout) {
        // cursors normally timeout after an inactivity period to prevent excess memory use
        // setting this prevents timeout of the cursor in question.
        _isNoTimeout = true;
        cursorStatsOpenNoTimeout.increment();
    }

    _cursorid = _cursorManager->registerCursor(this);

    cursorStatsOpen.increment();
    _countedYet = true;
}

ClientCursor::~ClientCursor() {
    if (_pos == -2) {
        // defensive: destructor called twice
        wassert(false);
        return;
    }

    invariant(!_isPinned);  // Must call unsetPinned() before invoking destructor.

    if (_countedYet) {
        _countedYet = false;
        cursorStatsOpen.decrement();
        if (_isNoTimeout)
            cursorStatsOpenNoTimeout.decrement();
    }

    if (_cursorManager) {
        // this could be null if kill() was killed
        _cursorManager->deregisterCursor(this);
    }

    // defensive:
    _cursorManager = NULL;
    _pos = -2;
    _isNoTimeout = false;
}

void ClientCursor::kill() {
    if (_exec.get())
        _exec->kill("cursor killed");

    _cursorManager = NULL;
}

//
// Timing and timeouts
//

bool ClientCursor::shouldTimeout(int millis) {
    _idleAgeMillis += millis;
    if (_isNoTimeout || _isPinned) {
        return false;
    }
    return _idleAgeMillis > cursorTimeoutMillis;
}

void ClientCursor::setIdleTime(int millis) {
    _idleAgeMillis = millis;
}

void ClientCursor::updateSlaveLocation(OperationContext* txn) {
    if (_slaveReadTill.isNull())
        return;

    verify(str::startsWith(_ns.c_str(), "local.oplog."));

    Client* c = txn->getClient();
    verify(c);
    OID rid = repl::ReplClientInfo::forClient(c).getRemoteID();
    if (!rid.isSet())
        return;

    repl::getGlobalReplicationCoordinator()->setLastOptimeForSlave(rid, _slaveReadTill);
}

//
// Pin methods
//

ClientCursorPin::ClientCursorPin(CursorManager* cursorManager, long long cursorid) : _cursor(NULL) {
    cursorStatsOpenPinned.increment();
    _cursor = cursorManager->find(cursorid, true);
}

ClientCursorPin::~ClientCursorPin() {
    cursorStatsOpenPinned.decrement();
    release();
}

void ClientCursorPin::release() {
    if (!_cursor)
        return;

    invariant(_cursor->isPinned());

    if (_cursor->cursorManager() == NULL) {
        // The ClientCursor was killed while we had it.  Therefore, it is our responsibility to
        // kill it.
        deleteUnderlying();
    } else {
        // Unpin the cursor under the collection cursor manager lock.
        _cursor->cursorManager()->unpin(_cursor);
    }

    _cursor = NULL;
}

void ClientCursorPin::deleteUnderlying() {
    invariant(_cursor);
    invariant(_cursor->isPinned());
    // Note the following subtleties of this method's implementation:
    // - We must unpin the cursor before destruction, since it is an error to destroy a pinned
    //   cursor.
    // - In addition, we must deregister the cursor before unpinning, since it is an
    //   error to unpin a registered cursor without holding the cursor manager lock (note that
    //   we can't simply unpin with the cursor manager lock here, since we need to guarantee
    //   exclusive ownership of the cursor when we are deleting it).
    if (_cursor->cursorManager()) {
        _cursor->cursorManager()->deregisterCursor(_cursor);
        _cursor->kill();
    }
    _cursor->unsetPinned();
    delete _cursor;
    _cursor = NULL;
}

ClientCursor* ClientCursorPin::c() const {
    return _cursor;
}

//
// ClientCursorMonitor
//

/**
 * Thread for timing out old cursors
 */
class ClientCursorMonitor : public BackgroundJob {
public:
    std::string name() const {
        return "ClientCursorMonitor";
    }

    void run() {
        Client::initThread("clientcursormon");
        Timer t;
        while (!inShutdown()) {
            {
                const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
                OperationContext& txn = *txnPtr;
                cursorStatsTimedOut.increment(
                    CursorManager::timeoutCursorsGlobal(&txn, t.millisReset()));
            }
            sleepsecs(clientCursorMonitorFrequencySecs);
        }
    }
};

namespace {
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
