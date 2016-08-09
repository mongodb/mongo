// counters.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/stats/counters.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;

OpCounters::OpCounters() {}

void OpCounters::gotInserts(int n) {
    RARELY _checkWrap();
    _insert.fetchAndAdd(n);
}

void OpCounters::gotInsert() {
    RARELY _checkWrap();
    _insert.fetchAndAdd(1);
}

void OpCounters::gotQuery() {
    RARELY _checkWrap();
    _query.fetchAndAdd(1);
}

void OpCounters::gotUpdate() {
    RARELY _checkWrap();
    _update.fetchAndAdd(1);
}

void OpCounters::gotDelete() {
    RARELY _checkWrap();
    _delete.fetchAndAdd(1);
}

void OpCounters::gotGetMore() {
    RARELY _checkWrap();
    _getmore.fetchAndAdd(1);
}

void OpCounters::gotCommand() {
    RARELY _checkWrap();
    _command.fetchAndAdd(1);
}

void OpCounters::gotOp(int op, bool isCommand) {
    switch (op) {
        case dbInsert: /*gotInsert();*/
            break;     // need to handle multi-insert
        case dbQuery:
            if (isCommand)
                gotCommand();
            else
                gotQuery();
            break;

        case dbUpdate:
            gotUpdate();
            break;
        case dbDelete:
            gotDelete();
            break;
        case dbGetMore:
            gotGetMore();
            break;
        case dbKillCursors:
        case opReply:
        case dbMsg:
            break;
        default:
            log() << "OpCounters::gotOp unknown op: " << op << endl;
    }
}

void OpCounters::_checkWrap() {
    const unsigned MAX = 1 << 30;

    bool wrap = _insert.loadRelaxed() > MAX || _query.loadRelaxed() > MAX ||
        _update.loadRelaxed() > MAX || _delete.loadRelaxed() > MAX ||
        _getmore.loadRelaxed() > MAX || _command.loadRelaxed() > MAX;

    if (wrap) {
        _insert.store(0);
        _query.store(0);
        _update.store(0);
        _delete.store(0);
        _getmore.store(0);
        _command.store(0);
    }
}

BSONObj OpCounters::getObj() const {
    BSONObjBuilder b;
    b.append("insert", _insert.loadRelaxed());
    b.append("query", _query.loadRelaxed());
    b.append("update", _update.loadRelaxed());
    b.append("delete", _delete.loadRelaxed());
    b.append("getmore", _getmore.loadRelaxed());
    b.append("command", _command.loadRelaxed());
    return b.obj();
}

void NetworkCounter::hitPhysical(long long bytesIn, long long bytesOut) {
    const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    bool overflow = _physicalBytesIn.loadRelaxed() > MAX || _physicalBytesOut.loadRelaxed() > MAX;

    if (overflow) {
        _physicalBytesIn.store(bytesIn);
        _physicalBytesOut.store(bytesOut);
    } else {
        _physicalBytesIn.fetchAndAdd(bytesIn);
        _physicalBytesOut.fetchAndAdd(bytesOut);
    }
}

void NetworkCounter::hitLogical(long long bytesIn, long long bytesOut) {
    const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    bool overflow = _logicalBytesIn.loadRelaxed() > MAX || _logicalBytesOut.loadRelaxed() > MAX;

    if (overflow) {
        _logicalBytesIn.store(bytesIn);
        _logicalBytesOut.store(bytesOut);
        // The requests field only gets incremented here (and not in hitPhysical) because the
        // hitLogical and hitPhysical are each called for each operation. Incrementing it in both
        // functions would double-count the number of operations.
        _requests.store(1);
    } else {
        _logicalBytesIn.fetchAndAdd(bytesIn);
        _logicalBytesOut.fetchAndAdd(bytesOut);
        _requests.fetchAndAdd(1);
    }
}

void NetworkCounter::append(BSONObjBuilder& b) {
    b.append("bytesIn", static_cast<long long>(_logicalBytesIn.loadRelaxed()));
    b.append("bytesOut", static_cast<long long>(_logicalBytesOut.loadRelaxed()));
    b.append("physicalBytesIn", static_cast<long long>(_physicalBytesIn.loadRelaxed()));
    b.append("physicalBytesOut", static_cast<long long>(_physicalBytesOut.loadRelaxed()));
    b.append("numRequests", static_cast<long long>(_requests.loadRelaxed()));
}


OpCounters globalOpCounters;
OpCounters replOpCounters;
NetworkCounter networkCounter;
}
