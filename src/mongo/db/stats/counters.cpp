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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/stats/counters.h"

#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"

namespace mongo {

void OpCounters::_checkWrap(CacheAligned<AtomicWord<long long>> OpCounters::*counter, int n) {
    static constexpr auto maxCount = 1LL << 60;
    auto oldValue = (this->*counter).fetchAndAddRelaxed(n);
    if (oldValue > maxCount) {
        _insert.store(0);
        _query.store(0);
        _update.store(0);
        _delete.store(0);
        _getmore.store(0);
        _command.store(0);

        _insertDeprecated.store(0);
        _queryDeprecated.store(0);
        _updateDeprecated.store(0);
        _deleteDeprecated.store(0);
        _getmoreDeprecated.store(0);
        _killcursorsDeprecated.store(0);
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

    auto queryDep = _queryDeprecated.loadRelaxed();
    auto getmoreDep = _getmoreDeprecated.loadRelaxed();
    auto killcursorsDep = _killcursorsDeprecated.loadRelaxed();
    auto updateDep = _updateDeprecated.loadRelaxed();
    auto deleteDep = _deleteDeprecated.loadRelaxed();
    auto insertDep = _insertDeprecated.loadRelaxed();
    auto totalDep = queryDep + getmoreDep + killcursorsDep + updateDep + deleteDep + insertDep;

    if (totalDep > 0) {
        BSONObjBuilder d(b.subobjStart("deprecated"));

        d.append("total", totalDep);
        d.append("insert", insertDep);
        d.append("query", queryDep);
        d.append("update", updateDep);
        d.append("delete", deleteDep);
        d.append("getmore", getmoreDep);
        d.append("killcursors", killcursorsDep);
    }

    return b.obj();
}

void NetworkCounter::hitPhysicalIn(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _physicalBytesIn.loadRelaxed() > MAX;

    if (overflow) {
        _physicalBytesIn.store(bytes);
    } else {
        _physicalBytesIn.fetchAndAdd(bytes);
    }
}

void NetworkCounter::hitPhysicalOut(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _physicalBytesOut.loadRelaxed() > MAX;

    if (overflow) {
        _physicalBytesOut.store(bytes);
    } else {
        _physicalBytesOut.fetchAndAdd(bytes);
    }
}

void NetworkCounter::hitLogicalIn(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _together.logicalBytesIn.loadRelaxed() > MAX;

    if (overflow) {
        _together.logicalBytesIn.store(bytes);
        // The requests field only gets incremented here (and not in hitPhysical) because the
        // hitLogical and hitPhysical are each called for each operation. Incrementing it in both
        // functions would double-count the number of operations.
        _together.requests.store(1);
    } else {
        _together.logicalBytesIn.fetchAndAdd(bytes);
        _together.requests.fetchAndAdd(1);
    }
}

void NetworkCounter::hitLogicalOut(long long bytes) {
    static const int64_t MAX = 1ULL << 60;

    // don't care about the race as its just a counter
    const bool overflow = _logicalBytesOut.loadRelaxed() > MAX;

    if (overflow) {
        _logicalBytesOut.store(bytes);
    } else {
        _logicalBytesOut.fetchAndAdd(bytes);
    }
}

void NetworkCounter::append(BSONObjBuilder& b) {
    b.append("bytesIn", static_cast<long long>(_together.logicalBytesIn.loadRelaxed()));
    b.append("bytesOut", static_cast<long long>(_logicalBytesOut.loadRelaxed()));
    b.append("physicalBytesIn", static_cast<long long>(_physicalBytesIn.loadRelaxed()));
    b.append("physicalBytesOut", static_cast<long long>(_physicalBytesOut.loadRelaxed()));
    b.append("numRequests", static_cast<long long>(_together.requests.loadRelaxed()));
}


OpCounters globalOpCounters;
OpCounters replOpCounters;
NetworkCounter networkCounter;
AggStageCounters aggStageCounters;
OperatorCountersMatchExpressions operatorCountersMatchExpressions;
}  // namespace mongo
