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

#include "mongo/platform/basic.h"

#include "mongo/db/server_transactions_metrics.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transactions_stats_gen.h"

namespace mongo {
namespace {
const auto ServerTransactionsMetricsDecoration =
    ServiceContext::declareDecoration<ServerTransactionsMetrics>();
}  // namespace

ServerTransactionsMetrics* ServerTransactionsMetrics::get(ServiceContext* service) {
    return &ServerTransactionsMetricsDecoration(service);
}

ServerTransactionsMetrics* ServerTransactionsMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

unsigned long long ServerTransactionsMetrics::getCurrentActive() const {
    return _currentActive.load();
}

void ServerTransactionsMetrics::decrementCurrentActive() {
    _currentActive.fetchAndSubtract(1);
}

void ServerTransactionsMetrics::incrementCurrentActive() {
    _currentActive.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentInactive() const {
    return _currentInactive.load();
}

void ServerTransactionsMetrics::decrementCurrentInactive() {
    _currentInactive.fetchAndSubtract(1);
}

void ServerTransactionsMetrics::incrementCurrentInactive() {
    _currentInactive.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentOpen() const {
    return _currentOpen.load();
}

void ServerTransactionsMetrics::decrementCurrentOpen() {
    _currentOpen.fetchAndSubtract(1);
}

void ServerTransactionsMetrics::incrementCurrentOpen() {
    _currentOpen.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalStarted() const {
    return _totalStarted.load();
}

void ServerTransactionsMetrics::incrementTotalStarted() {
    _totalStarted.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalAborted() const {
    return _totalAborted.load();
}

void ServerTransactionsMetrics::incrementTotalAborted() {
    _totalAborted.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalCommitted() const {
    return _totalCommitted.load();
}

void ServerTransactionsMetrics::incrementTotalCommitted() {
    _totalCommitted.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPrepared() const {
    return _totalPrepared.load();
}

void ServerTransactionsMetrics::incrementTotalPrepared() {
    _totalPrepared.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenCommitted() const {
    return _totalPreparedThenCommitted.load();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenCommitted() {
    _totalPreparedThenCommitted.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenAborted() const {
    return _totalPreparedThenAborted.load();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenAborted() {
    _totalPreparedThenAborted.fetchAndAdd(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentPrepared() const {
    return _currentPrepared.load();
}

void ServerTransactionsMetrics::incrementCurrentPrepared() {
    _currentPrepared.fetchAndAdd(1);
}

void ServerTransactionsMetrics::decrementCurrentPrepared() {
    _currentPrepared.fetchAndSubtract(1);
}

boost::optional<repl::OpTime> ServerTransactionsMetrics::_calculateOldestActiveOpTime(
    WithLock) const {
    if (_oldestActiveOplogEntryOpTimes.empty()) {
        return boost::none;
    }
    return *(_oldestActiveOplogEntryOpTimes.begin());
}

void ServerTransactionsMetrics::addActiveOpTime(repl::OpTime oldestOplogEntryOpTime) {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    auto ret = _oldestActiveOplogEntryOpTimes.insert(oldestOplogEntryOpTime);
    // If ret.second is false, the OpTime we tried to insert already existed.
    invariant(ret.second,
              str::stream() << "This oplog entry OpTime already exists in "
                            << "oldestActiveOplogEntryOpTimes."
                            << "oldestOplogEntryOpTime: "
                            << oldestOplogEntryOpTime.toString());

    // Add this OpTime to the oldestNonMajorityCommittedOpTimes set with a finishOpTime of
    // Timestamp::max() to signify that it has not been committed/aborted.
    std::pair<repl::OpTime, repl::OpTime> nonMajCommittedOpTime(oldestOplogEntryOpTime,
                                                                repl::OpTime::max());
    auto ret2 = _oldestNonMajorityCommittedOpTimes.insert(nonMajCommittedOpTime);
    // If ret2.second is false, the OpTime we tried to insert already existed.
    invariant(ret2.second,
              str::stream() << "This oplog entry OpTime already exists in "
                            << "oldestNonMajorityCommittedOpTimes."
                            << "oldestOplogEntryOpTime: "
                            << oldestOplogEntryOpTime.toString());
    _oldestActiveOplogEntryOpTime = _calculateOldestActiveOpTime(lm);
}

void ServerTransactionsMetrics::removeActiveOpTime(repl::OpTime oldestOplogEntryOpTime,
                                                   boost::optional<repl::OpTime> finishOpTime) {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    auto it = _oldestActiveOplogEntryOpTimes.find(oldestOplogEntryOpTime);
    invariant(it != _oldestActiveOplogEntryOpTimes.end(),
              str::stream() << "This oplog entry OpTime does not exist in or has already been "
                            << "removed from oldestActiveOplogEntryOpTimes."
                            << "OpTime: "
                            << oldestOplogEntryOpTime.toString());
    _oldestActiveOplogEntryOpTimes.erase(it);

    if (!finishOpTime) {
        return;
    }

    // The transaction's oldestOplogEntryOpTime now has a corresponding finishTime, which will
    // be its commit or abort oplog entry OpTime. Add this pair to the
    // oldestNonMajorityCommittedOpTimes.
    // Currently, the oldestOplogEntryOpTime will be a prepareOpTime so we will only have a
    // finishOpTime if we are committing a prepared transaction or aborting an active prepared
    // transaction.
    std::pair<repl::OpTime, repl::OpTime> opTimeToRemove(oldestOplogEntryOpTime,
                                                         repl::OpTime::max());
    auto it2 = _oldestNonMajorityCommittedOpTimes.find(opTimeToRemove);
    invariant(it2 != _oldestNonMajorityCommittedOpTimes.end(),
              str::stream() << "This oplog entry OpTime does not exist in or has already been "
                            << "removed from oldestNonMajorityCommittedOpTimes"
                            << "oldestOplogEntryOpTime: "
                            << oldestOplogEntryOpTime.toString());
    _oldestNonMajorityCommittedOpTimes.erase(it2);

    std::pair<repl::OpTime, repl::OpTime> nonMajCommittedOpTime(oldestOplogEntryOpTime,
                                                                *finishOpTime);
    auto ret = _oldestNonMajorityCommittedOpTimes.insert(nonMajCommittedOpTime);
    // If ret.second is false, the OpTime we tried to insert already existed.
    invariant(ret.second,
              str::stream() << "This oplog entry OpTime pair already exists in "
                            << "oldestNonMajorityCommittedOpTimes."
                            << "oldestOplogEntryOpTime: "
                            << oldestOplogEntryOpTime.toString()
                            << "finishOpTime: "
                            << finishOpTime->toString());
    _oldestActiveOplogEntryOpTime = _calculateOldestActiveOpTime(lm);
}

boost::optional<repl::OpTime> ServerTransactionsMetrics::getOldestNonMajorityCommittedOpTime()
    const {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    if (_oldestNonMajorityCommittedOpTimes.empty()) {
        return boost::none;
    }
    const auto oldestNonMajorityCommittedOpTime = _oldestNonMajorityCommittedOpTimes.begin()->first;
    invariant(!oldestNonMajorityCommittedOpTime.isNull());
    return oldestNonMajorityCommittedOpTime;
}

void ServerTransactionsMetrics::removeOpTimesLessThanOrEqToCommittedOpTime(
    repl::OpTime committedOpTime) {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    // Iterate through oldestNonMajorityCommittedOpTimes and remove all pairs whose "finishOpTime"
    // is now less than or equal to the commit point.
    for (auto it = _oldestNonMajorityCommittedOpTimes.begin();
         it != _oldestNonMajorityCommittedOpTimes.end();) {
        if (it->second <= committedOpTime) {
            it = _oldestNonMajorityCommittedOpTimes.erase(it);
        } else {
            ++it;
        }
    }
}

boost::optional<repl::OpTime>
ServerTransactionsMetrics::getFinishOpTimeOfOldestNonMajCommitted_forTest() const {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    if (_oldestNonMajorityCommittedOpTimes.empty()) {
        return boost::none;
    }
    const auto oldestNonMajorityCommittedOpTime =
        _oldestNonMajorityCommittedOpTimes.begin()->second;
    return oldestNonMajorityCommittedOpTime;
}

boost::optional<repl::OpTime> ServerTransactionsMetrics::getOldestActiveOpTime() const {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    return _oldestActiveOplogEntryOpTime;
}

unsigned int ServerTransactionsMetrics::getTotalActiveOpTimes() const {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    return _oldestActiveOplogEntryOpTimes.size();
}

Timestamp ServerTransactionsMetrics::_getOldestOpenUnpreparedReadTimestamp(
    OperationContext* opCtx) {
    // The history is not pinned in memory once a transaction has been prepared since reads
    // are no longer possible. Therefore, the timestamp returned by the storage engine refers
    // to the oldest read timestamp for any open unprepared transaction.
    return opCtx->getServiceContext()->getStorageEngine()->getOldestOpenReadTimestamp();
}

void ServerTransactionsMetrics::updateStats(TransactionsStats* stats, OperationContext* opCtx) {
    stats->setCurrentActive(_currentActive.load());
    stats->setCurrentInactive(_currentInactive.load());
    stats->setCurrentOpen(_currentOpen.load());
    stats->setTotalAborted(_totalAborted.load());
    stats->setTotalCommitted(_totalCommitted.load());
    stats->setTotalStarted(_totalStarted.load());
    stats->setTotalPrepared(_totalPrepared.load());
    stats->setTotalPreparedThenCommitted(_totalPreparedThenCommitted.load());
    stats->setTotalPreparedThenAborted(_totalPreparedThenAborted.load());
    stats->setCurrentPrepared(_currentPrepared.load());
    stats->setOldestOpenUnpreparedReadTimestamp(
        ServerTransactionsMetrics::_getOldestOpenUnpreparedReadTimestamp(opCtx));
    // Acquire _mutex before reading _oldestActiveOplogEntryOpTime.
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    // To avoid compression loss, we have Timestamp(0, 0) be the default value if no oldest active
    // transaction optime is stored.
    Timestamp oldestActiveOplogEntryTimestamp = (_oldestActiveOplogEntryOpTime != boost::none)
        ? _oldestActiveOplogEntryOpTime->getTimestamp()
        : Timestamp();
    stats->setOldestActiveOplogEntryTimestamp(oldestActiveOplogEntryTimestamp);
}

void ServerTransactionsMetrics::clearOpTimes() {
    stdx::lock_guard<stdx::mutex> lm(_mutex);
    _oldestActiveOplogEntryOpTime = boost::none;
    _oldestActiveOplogEntryOpTimes.clear();
    _oldestNonMajorityCommittedOpTimes.clear();
}

namespace {
class TransactionsSSS : public ServerStatusSection {
public:
    TransactionsSSS() : ServerStatusSection("transactions") {}

    ~TransactionsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        TransactionsStats stats;

        // Retryable writes and multi-document transactions metrics are both included in the same
        // serverStatus section because both utilize similar internal machinery for tracking their
        // lifecycle within a session. Both are assigned transaction numbers, and so both are often
        // referred to as “transactions”.
        RetryableWritesStats::get(opCtx)->updateStats(&stats);
        ServerTransactionsMetrics::get(opCtx)->updateStats(&stats, opCtx);
        return stats.toBSON();
    }

} transactionsSSS;
}  // namespace

}  // namespace mongo
