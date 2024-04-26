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

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "mongo/db/stats/top.h"

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo {
namespace {

const auto getTop = ServiceContext::declareDecoration<Top>();

template <typename HistogramType>
void incrementHistogram(OperationContext* opCtx,
                        long long latency,
                        HistogramType& histogram,
                        Command::ReadWriteType readWriteType) {
    const auto isQueryableEncryptionOperation = [&] {
        auto curOp = CurOp::get(opCtx);
        while (curOp) {
            if (curOp->getShouldOmitDiagnosticInformation()) {
                return true;
            }
            curOp = curOp->parent();
        }
        return false;
    }();
    histogram.increment(latency, readWriteType, isQueryableEncryptionOperation);
}

template <typename HistogramType>
void incrementHistogramForUser(OperationContext* opCtx,
                               long long latency,
                               HistogramType& histogram,
                               Command::ReadWriteType readWriteType) {
    if (auto c = opCtx->getClient(); !c->isFromUserConnection() || c->isInDirectClient()) {
        // Only update histogram if operation came from a user.
        return;
    }
    incrementHistogram(opCtx, latency, histogram, readWriteType);
}

void updateCollectionData(WithLock,
                          OperationContext* opCtx,
                          Top::CollectionData& c,
                          LogicalOp logicalOp,
                          Top::LockType lockType,
                          long long micros,
                          Command::ReadWriteType readWriteType) {
    if (c.isStatsRecordingAllowed) {
        c.isStatsRecordingAllowed = !CurOp::get(opCtx)->getShouldOmitDiagnosticInformation();
    }

    incrementHistogramForUser(opCtx, micros, c.opLatencyHistogram, readWriteType);

    c.total.inc(micros);

    if (lockType == Top::LockType::WriteLocked)
        c.writeLock.inc(micros);
    else if (lockType == Top::LockType::ReadLocked)
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
}  // namespace

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
    stdx::lock_guard lk(_lockUsage);
    CollectionData& coll = _usage[hashedNs];
    updateCollectionData(lk, opCtx, coll, logicalOp, lockType, micros, readWriteType);
}

void Top::record(OperationContext* opCtx,
                 std::span<const NamespaceString> nssSet,
                 LogicalOp logicalOp,
                 LockType lockType,
                 long long micros,
                 bool command,
                 Command::ReadWriteType readWriteType) {
    std::vector<std::string> hashedSet;
    hashedSet.reserve(nssSet.size());
    for (auto& nss : nssSet) {
        const auto nssStr =
            NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
        if (nssStr[0] != '?') {
            hashedSet.emplace_back(UsageMap::hasher().hashed_key(nssStr));
        }
    }

    stdx::lock_guard lk(_lockUsage);
    for (const auto& hashedNs : hashedSet) {
        CollectionData& coll = _usage[hashedNs];
        updateCollectionData(lk, opCtx, coll, logicalOp, lockType, micros, readWriteType);
    }
}

void Top::collectionDropped(const NamespaceString& nss) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    stdx::lock_guard lk(_lockUsage);
    _usage.erase(nssStr);
}

void Top::append(BSONObjBuilder& b) {
    stdx::lock_guard lk(_lockUsage);

    auto appendStatsEntry = [&](BSONObjBuilder& b, auto name, const UsageData& data) {
        BSONObjBuilder bb(b.subobjStart(name));
        bb.appendNumber("time", data.time);
        bb.appendNumber("count", data.count);
        bb.done();
    };

    // pull all the names into a vector so we can sort them for the user
    std::vector<std::string> names;
    for (UsageMap::const_iterator i = _usage.begin(); i != _usage.end(); ++i) {
        names.push_back(i->first);
    }

    std::sort(names.begin(), names.end());

    for (size_t i = 0; i < names.size(); i++) {
        BSONObjBuilder bb(b.subobjStart(names[i]));

        const CollectionData& coll = _usage.find(names[i])->second;
        auto pos = names[i].find('.');
        if (coll.isStatsRecordingAllowed &&
            !NamespaceString::isFLE2StateCollection(names[i].substr(pos + 1))) {
            appendStatsEntry(b, "total", coll.total);

            appendStatsEntry(b, "readLock", coll.readLock);
            appendStatsEntry(b, "writeLock", coll.writeLock);

            appendStatsEntry(b, "queries", coll.queries);
            appendStatsEntry(b, "getmore", coll.getmore);
            appendStatsEntry(b, "insert", coll.insert);
            appendStatsEntry(b, "update", coll.update);
            appendStatsEntry(b, "remove", coll.remove);
            appendStatsEntry(b, "commands", coll.commands);
        }
        bb.done();
    }
}

void Top::appendLatencyStats(const NamespaceString& nss,
                             bool includeHistograms,
                             BSONObjBuilder* builder) {
    const auto nssStr = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    auto hashedNs = UsageMap::hasher().hashed_key(nssStr);
    stdx::lock_guard lk(_lockUsage);
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

    incrementHistogramForUser(opCtx, latency, _globalHistogramStats, readWriteType);
}

void Top::appendGlobalLatencyStats(bool includeHistograms,
                                   bool slowMSBucketsOnly,
                                   BSONObjBuilder* builder) {
    _globalHistogramStats.append(includeHistograms, slowMSBucketsOnly, builder);
}

void Top::incrementGlobalTransactionLatencyStats(OperationContext* opCtx, uint64_t latency) {
    incrementHistogram(opCtx, latency, _globalHistogramStats, Command::ReadWriteType::kTransaction);
}
}  // namespace mongo
