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


#include <absl/container/node_hash_map.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using std::endl;
using std::string;
using std::stringstream;
using std::vector;

namespace {

const auto getTop = ServiceContext::declareDecoration<Top>();

bool isQuerableEncryptionOperation(OperationContext* opCtx) {
    auto curop = CurOp::get(opCtx);

    while (curop != nullptr) {
        if (curop->getShouldOmitDiagnosticInformation()) {
            return true;
        }

        curop = curop->parent();
    }

    return false;
}

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

void Top::record(OperationContext* opCtx,
                 const NamespaceString& nss,
                 LogicalOp logicalOp,
                 LockType lockType,
                 long long micros,
                 bool command,
                 Command::ReadWriteType readWriteType) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    if (nssStr[0] == '?')
        return;

    auto hashedNs = UsageMap::hasher().hashed_key(nssStr);
    stdx::lock_guard<Latch> lk(_lockUsage);

    CollectionData& coll = _usage[hashedNs];
    _record(opCtx, coll, logicalOp, lockType, micros, readWriteType);
}

void Top::record(OperationContext* opCtx,
                 const std::set<NamespaceString>& nssSet,
                 LogicalOp logicalOp,
                 LockType lockType,
                 long long micros,
                 bool command,
                 Command::ReadWriteType readWriteType) {
    for (const auto& nss : nssSet) {
        record(opCtx, nss, logicalOp, lockType, micros, command, readWriteType);
    }
}

void Top::_record(OperationContext* opCtx,
                  CollectionData& c,
                  LogicalOp logicalOp,
                  LockType lockType,
                  long long micros,
                  Command::ReadWriteType readWriteType) {
    if (c.isStatsRecordingAllowed) {
        c.isStatsRecordingAllowed = !CurOp::get(opCtx)->getShouldOmitDiagnosticInformation();
    }

    _incrementHistogram(opCtx, micros, &c.opLatencyHistogram, readWriteType);

    c.total.inc(micros);

    if (lockType == LockType::WriteLocked)
        c.writeLock.inc(micros);
    else if (lockType == LockType::ReadLocked)
        c.readLock.inc(micros);

    switch (logicalOp) {
        case LogicalOp::opInvalid:
            // use 0 for unknown, non-specific
            break;
        case LogicalOp::opBulkWrite:
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

void Top::collectionDropped(const NamespaceString& nss) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    stdx::lock_guard<Latch> lk(_lockUsage);
    _usage.erase(nssStr);
}

void Top::append(BSONObjBuilder& b) {
    stdx::lock_guard<Latch> lk(_lockUsage);
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
        auto pos = names[i].find('.');
        if (coll.isStatsRecordingAllowed &&
            !NamespaceString::isFLE2StateCollection(names[i].substr(pos + 1))) {
            _appendStatsEntry(b, "total", coll.total);

            _appendStatsEntry(b, "readLock", coll.readLock);
            _appendStatsEntry(b, "writeLock", coll.writeLock);

            _appendStatsEntry(b, "queries", coll.queries);
            _appendStatsEntry(b, "getmore", coll.getmore);
            _appendStatsEntry(b, "insert", coll.insert);
            _appendStatsEntry(b, "update", coll.update);
            _appendStatsEntry(b, "remove", coll.remove);
            _appendStatsEntry(b, "commands", coll.commands);
        }
        bb.done();
    }
}

void Top::_appendStatsEntry(BSONObjBuilder& b, const char* statsName, const UsageData& map) const {
    BSONObjBuilder bb(b.subobjStart(statsName));
    bb.appendNumber("time", map.time);
    bb.appendNumber("count", map.count);
    bb.done();
}

void Top::appendLatencyStats(const NamespaceString& nss,
                             bool includeHistograms,
                             BSONObjBuilder* builder) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    auto hashedNs = UsageMap::hasher().hashed_key(nssStr);
    stdx::lock_guard<Latch> lk(_lockUsage);
    BSONObjBuilder latencyStatsBuilder;
    _usage[hashedNs].opLatencyHistogram.append(includeHistograms, false, &latencyStatsBuilder);
    builder->append("ns", nssStr);
    builder->append("latencyStats", latencyStatsBuilder.obj());
}

void Top::incrementGlobalLatencyStats(OperationContext* opCtx,
                                      uint64_t latency,
                                      Command::ReadWriteType readWriteType) {
    if (!opCtx->shouldIncrementLatencyStats())
        return;

    stdx::lock_guard<Latch> guard(_lockGlobal);
    _incrementHistogram(opCtx, latency, &_globalHistogramStats, readWriteType);
}

void Top::appendGlobalLatencyStats(bool includeHistograms,
                                   bool slowMSBucketsOnly,
                                   BSONObjBuilder* builder) {
    stdx::lock_guard<Latch> guard(_lockGlobal);
    _globalHistogramStats.append(includeHistograms, slowMSBucketsOnly, builder);
}

void Top::incrementGlobalTransactionLatencyStats(OperationContext* opCtx, uint64_t latency) {
    stdx::lock_guard<Latch> guard(_lockGlobal);
    _globalHistogramStats.increment(
        latency, Command::ReadWriteType::kTransaction, isQuerableEncryptionOperation(opCtx));
}

void Top::_incrementHistogram(OperationContext* opCtx,
                              long long latency,
                              OperationLatencyHistogram* histogram,
                              Command::ReadWriteType readWriteType) {
    // Only update histogram if operation came from a user.
    Client* client = opCtx->getClient();
    if (client->isFromUserConnection() && !client->isInDirectClient()) {
        histogram->increment(latency, readWriteType, isQuerableEncryptionOperation(opCtx));
    }
}
}  // namespace mongo
