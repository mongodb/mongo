/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/s/router_transactions_metrics.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/router_transactions_stats_gen.h"
#include "mongo/s/transaction_router.h"

namespace mongo {
namespace {

const auto RouterTransactionsMetricsDecoration =
    ServiceContext::declareDecoration<RouterTransactionsMetrics>();

}  // namespace

RouterTransactionsMetrics* RouterTransactionsMetrics::get(ServiceContext* service) {
    return &RouterTransactionsMetricsDecoration(service);
}

RouterTransactionsMetrics* RouterTransactionsMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

std::int64_t RouterTransactionsMetrics::getTotalStarted() {
    return _totalStarted.load();
}

void RouterTransactionsMetrics::incrementTotalStarted() {
    _totalStarted.fetchAndAdd(1);
}

std::int64_t RouterTransactionsMetrics::getTotalAborted() {
    return _totalAborted.load();
}

void RouterTransactionsMetrics::incrementTotalAborted() {
    _totalAborted.fetchAndAdd(1);
}

std::int64_t RouterTransactionsMetrics::getTotalCommitted() {
    return _totalCommitted.load();
}

void RouterTransactionsMetrics::incrementTotalCommitted() {
    _totalCommitted.fetchAndAdd(1);
}

std::int64_t RouterTransactionsMetrics::getTotalContactedParticipants() {
    return _totalContactedParticipants.load();
}

void RouterTransactionsMetrics::incrementTotalContactedParticipants() {
    _totalContactedParticipants.fetchAndAdd(1);
}

std::int64_t RouterTransactionsMetrics::getTotalParticipantsAtCommit() {
    return _totalParticipantsAtCommit.load();
}

void RouterTransactionsMetrics::addToTotalParticipantsAtCommit(std::int64_t inc) {
    _totalParticipantsAtCommit.fetchAndAdd(inc);
}

std::int64_t RouterTransactionsMetrics::getTotalRequestsTargeted() {
    return _totalRequestsTargeted.load();
}

void RouterTransactionsMetrics::incrementTotalRequestsTargeted() {
    _totalRequestsTargeted.fetchAndAdd(1);
}

const RouterTransactionsMetrics::CommitStats& RouterTransactionsMetrics::getCommitTypeStats_forTest(
    TransactionRouter::CommitType commitType) {
    switch (commitType) {
        case TransactionRouter::CommitType::kNotInitiated:
            break;
        case TransactionRouter::CommitType::kNoShards:
            return _noShardsCommitStats;
        case TransactionRouter::CommitType::kSingleShard:
            return _singleShardCommitStats;
        case TransactionRouter::CommitType::kSingleWriteShard:
            return _singleWriteShardCommitStats;
        case TransactionRouter::CommitType::kReadOnly:
            return _readOnlyCommitStats;
        case TransactionRouter::CommitType::kTwoPhaseCommit:
            return _twoPhaseCommitStats;
        case TransactionRouter::CommitType::kRecoverWithToken:
            return _recoverWithTokenCommitStats;
    }
    MONGO_UNREACHABLE;
}

void RouterTransactionsMetrics::incrementCommitInitiated(TransactionRouter::CommitType commitType) {
    switch (commitType) {
        case TransactionRouter::CommitType::kNotInitiated:
            MONGO_UNREACHABLE;
        case TransactionRouter::CommitType::kNoShards:
            _noShardsCommitStats.initiated.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kSingleShard:
            _singleShardCommitStats.initiated.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kSingleWriteShard:
            _singleWriteShardCommitStats.initiated.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kReadOnly:
            _readOnlyCommitStats.initiated.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kTwoPhaseCommit:
            _twoPhaseCommitStats.initiated.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kRecoverWithToken:
            _recoverWithTokenCommitStats.initiated.fetchAndAdd(1);
            break;
    }
}

void RouterTransactionsMetrics::incrementCommitSuccessful(
    TransactionRouter::CommitType commitType) {
    switch (commitType) {
        case TransactionRouter::CommitType::kNotInitiated:
            MONGO_UNREACHABLE;
        case TransactionRouter::CommitType::kNoShards:
            _noShardsCommitStats.successful.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kSingleShard:
            _singleShardCommitStats.successful.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kSingleWriteShard:
            _singleWriteShardCommitStats.successful.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kReadOnly:
            _readOnlyCommitStats.successful.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kTwoPhaseCommit:
            _twoPhaseCommitStats.successful.fetchAndAdd(1);
            break;
        case TransactionRouter::CommitType::kRecoverWithToken:
            _recoverWithTokenCommitStats.successful.fetchAndAdd(1);
            break;
    }
}

void RouterTransactionsMetrics::incrementAbortCauseMap(std::string abortCause) {
    invariant(!abortCause.empty());

    stdx::lock_guard<stdx::mutex> lock(_abortCauseMutex);
    auto it = _abortCauseMap.find(abortCause);
    if (it == _abortCauseMap.end()) {
        _abortCauseMap.emplace(std::pair<std::string, std::int64_t>(std::move(abortCause), 1));
    } else {
        it->second++;
    }
}

CommitTypeStats RouterTransactionsMetrics::_constructCommitTypeStats(const CommitStats& stats) {
    CommitTypeStats commitStats;
    commitStats.setInitiated(stats.initiated.load());
    commitStats.setSuccessful(stats.successful.load());
    return commitStats;
}

void RouterTransactionsMetrics::updateStats(RouterTransactionsStats* stats) {
    stats->setTotalStarted(_totalStarted.load());
    stats->setTotalCommitted(_totalCommitted.load());
    stats->setTotalAborted(_totalAborted.load());
    stats->setTotalContactedParticipants(_totalContactedParticipants.load());
    stats->setTotalParticipantsAtCommit(_totalParticipantsAtCommit.load());
    stats->setTotalRequestsTargeted(_totalRequestsTargeted.load());

    CommitTypes commitTypes;
    commitTypes.setNoShards(_constructCommitTypeStats(_noShardsCommitStats));
    commitTypes.setSingleShard(_constructCommitTypeStats(_singleShardCommitStats));
    commitTypes.setSingleWriteShard(_constructCommitTypeStats(_singleWriteShardCommitStats));
    commitTypes.setReadOnly(_constructCommitTypeStats(_readOnlyCommitStats));
    commitTypes.setTwoPhaseCommit(_constructCommitTypeStats(_twoPhaseCommitStats));
    commitTypes.setRecoverWithToken(_constructCommitTypeStats(_recoverWithTokenCommitStats));
    stats->setCommitTypes(commitTypes);

    BSONObjBuilder bob;
    {
        stdx::lock_guard<stdx::mutex> lock(_abortCauseMutex);
        for (auto const& abortCauseEntry : _abortCauseMap) {
            bob.append(abortCauseEntry.first, abortCauseEntry.second);
        }
    }
    stats->setAbortCause(bob.obj());
}

}  // namespace mongo
