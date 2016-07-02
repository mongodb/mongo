// top.cpp
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

#include "mongo/db/stats/top.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

namespace {

const auto getTop = ServiceContext::declareDecoration<Top>();

}  // namespace

Top::UsageData::UsageData(const UsageData& older, const UsageData& newer) {
    // this won't be 100% accurate on rollovers and drop(), but at least it won't be negative
    time = (newer.time >= older.time) ? (newer.time - older.time) : newer.time;
    count = (newer.count >= older.count) ? (newer.count - older.count) : newer.count;
}

Top::CollectionData::CollectionData(const CollectionData& older, const CollectionData& newer)
    : total(older.total, newer.total),
      readLock(older.readLock, newer.readLock),
      writeLock(older.writeLock, newer.writeLock),
      queries(older.queries, newer.queries),
      getmore(older.getmore, newer.getmore),
      insert(older.insert, newer.insert),
      update(older.update, newer.update),
      remove(older.remove, newer.remove),
      commands(older.commands, newer.commands) {}

// static
Top& Top::get(ServiceContext* service) {
    return getTop(service);
}

void Top::record(OperationContext* txn,
                 StringData ns,
                 LogicalOp logicalOp,
                 int lockType,
                 long long micros,
                 bool command,
                 Command::ReadWriteType readWriteType) {
    if (ns[0] == '?')
        return;

    auto hashedNs = UsageMap::HashedKey(ns);

    // cout << "record: " << ns << "\t" << op << "\t" << command << endl;
    stdx::lock_guard<SimpleMutex> lk(_lock);

    if ((command || logicalOp == LogicalOp::opQuery) && ns == _lastDropped) {
        _lastDropped = "";
        return;
    }

    CollectionData& coll = _usage[hashedNs];
    _record(txn, coll, logicalOp, lockType, micros, readWriteType);
}

void Top::_record(OperationContext* txn,
                  CollectionData& c,
                  LogicalOp logicalOp,
                  int lockType,
                  long long micros,
                  Command::ReadWriteType readWriteType) {

    _incrementHistogram(txn, micros, &c.opLatencyHistogram, readWriteType);

    c.total.inc(micros);

    if (lockType > 0)
        c.writeLock.inc(micros);
    else if (lockType < 0)
        c.readLock.inc(micros);

    switch (logicalOp) {
        case LogicalOp::opInvalid:
            // use 0 for unknown, non-specific
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

void Top::collectionDropped(StringData ns) {
    stdx::lock_guard<SimpleMutex> lk(_lock);
    _usage.erase(ns);
    _lastDropped = ns.toString();
}

void Top::cloneMap(Top::UsageMap& out) const {
    stdx::lock_guard<SimpleMutex> lk(_lock);
    out = _usage;
}

void Top::append(BSONObjBuilder& b) {
    stdx::lock_guard<SimpleMutex> lk(_lock);
    _appendToUsageMap(b, _usage);
}

void Top::_appendToUsageMap(BSONObjBuilder& b, const UsageMap& map) const {
    // pull all the names into a vector so we can sort them for the user

    vector<string> names;
    for (UsageMap::const_iterator i = map.begin(); i != map.end(); ++i) {
        names.push_back(i->first);
    }

    std::sort(names.begin(), names.end());

    for (size_t i = 0; i < names.size(); i++) {
        BSONObjBuilder bb(b.subobjStart(names[i]));

        const CollectionData& coll = map.find(names[i])->second;

        _appendStatsEntry(b, "total", coll.total);

        _appendStatsEntry(b, "readLock", coll.readLock);
        _appendStatsEntry(b, "writeLock", coll.writeLock);

        _appendStatsEntry(b, "queries", coll.queries);
        _appendStatsEntry(b, "getmore", coll.getmore);
        _appendStatsEntry(b, "insert", coll.insert);
        _appendStatsEntry(b, "update", coll.update);
        _appendStatsEntry(b, "remove", coll.remove);
        _appendStatsEntry(b, "commands", coll.commands);

        bb.done();
    }
}

void Top::_appendStatsEntry(BSONObjBuilder& b, const char* statsName, const UsageData& map) const {
    BSONObjBuilder bb(b.subobjStart(statsName));
    bb.appendNumber("time", map.time);
    bb.appendNumber("count", map.count);
    bb.done();
}

void Top::appendLatencyStats(StringData ns, BSONObjBuilder* builder) {
    auto hashedNs = UsageMap::HashedKey(ns);
    stdx::lock_guard<SimpleMutex> lk(_lock);
    BSONObjBuilder latencyStatsBuilder;
    _usage[hashedNs].opLatencyHistogram.append(&latencyStatsBuilder);
    builder->append("latencyStats", latencyStatsBuilder.obj());
}

void Top::incrementGlobalLatencyStats(OperationContext* txn,
                                      uint64_t latency,
                                      Command::ReadWriteType readWriteType) {
    stdx::lock_guard<SimpleMutex> guard(_lock);
    _incrementHistogram(txn, latency, &_globalHistogramStats, readWriteType);
}

void Top::appendGlobalLatencyStats(BSONObjBuilder* builder) {
    stdx::lock_guard<SimpleMutex> guard(_lock);
    _globalHistogramStats.append(builder);
}

void Top::_incrementHistogram(OperationContext* txn,
                              long long latency,
                              OperationLatencyHistogram* histogram,
                              Command::ReadWriteType readWriteType) {
    // Only update histogram if operation came from a user.
    Client* client = txn->getClient();
    if (client->isFromUserConnection() && !client->isInDirectClient()) {
        histogram->increment(latency, readWriteType);
    }
}

/**
 * Appends the global histogram to the server status.
 */
class GlobalHistogramServerStatusMetric : public ServerStatusMetric {
public:
    GlobalHistogramServerStatusMetric() : ServerStatusMetric(".metrics.latency") {}
    virtual void appendAtLeaf(BSONObjBuilder& builder) const {
        BSONObjBuilder latencyBuilder;
        Top::get(getGlobalServiceContext()).appendGlobalLatencyStats(&latencyBuilder);
        builder.append("latency", latencyBuilder.obj());
    }
} globalHistogramServerStatusMetric;
}  // namespace mongo
