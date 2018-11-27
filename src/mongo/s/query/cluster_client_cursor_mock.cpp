
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_client_cursor_mock.h"

#include "mongo/util/assert_util.h"

namespace mongo {

ClusterClientCursorMock::ClusterClientCursorMock(boost::optional<LogicalSessionId> lsid,
                                                 boost::optional<TxnNumber> txnNumber,
                                                 stdx::function<void(void)> killCallback)
    : _killCallback(std::move(killCallback)), _lsid(lsid), _txnNumber(txnNumber) {}

ClusterClientCursorMock::~ClusterClientCursorMock() {
    invariant((_exhausted && _remotesExhausted) || _killed);
}

StatusWith<ClusterQueryResult> ClusterClientCursorMock::next(
    RouterExecStage::ExecContext execContext) {
    invariant(!_killed);

    if (_resultsQueue.empty()) {
        _exhausted = true;
        return {ClusterQueryResult()};
    }

    auto out = _resultsQueue.front();
    _resultsQueue.pop();

    if (!out.isOK()) {
        return out.getStatus();
    }

    ++_numReturnedSoFar;
    return out.getValue();
}

BSONObj ClusterClientCursorMock::getOriginatingCommand() const {
    return _originatingCommand;
}

std::size_t ClusterClientCursorMock::getNumRemotes() const {
    MONGO_UNREACHABLE;
}

BSONObj ClusterClientCursorMock::getPostBatchResumeToken() const {
    MONGO_UNREACHABLE;
}

long long ClusterClientCursorMock::getNumReturnedSoFar() const {
    return _numReturnedSoFar;
}

std::uint64_t ClusterClientCursorMock::getNBatches() const {
    return _nBatchesReturned;
}

void ClusterClientCursorMock::incNBatches() {
    ++_nBatchesReturned;
}

Date_t ClusterClientCursorMock::getCreatedDate() const {
    return _createdDate;
}

Date_t ClusterClientCursorMock::getLastUseDate() const {
    return _lastUseDate;
}

void ClusterClientCursorMock::setLastUseDate(Date_t now) {
    _lastUseDate = std::move(now);
}

void ClusterClientCursorMock::kill(OperationContext* opCtx) {
    _killed = true;
    if (_killCallback) {
        _killCallback();
    }
}

bool ClusterClientCursorMock::isTailable() const {
    return false;
}

bool ClusterClientCursorMock::isTailableAndAwaitData() const {
    return false;
}

void ClusterClientCursorMock::queueResult(const ClusterQueryResult& result) {
    _resultsQueue.push({result});
}

bool ClusterClientCursorMock::remotesExhausted() {
    return _remotesExhausted;
}

void ClusterClientCursorMock::markRemotesNotExhausted() {
    _remotesExhausted = false;
}

void ClusterClientCursorMock::queueError(Status status) {
    _resultsQueue.push({status});
}

Status ClusterClientCursorMock::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    MONGO_UNREACHABLE;
}

boost::optional<LogicalSessionId> ClusterClientCursorMock::getLsid() const {
    return _lsid;
}

boost::optional<TxnNumber> ClusterClientCursorMock::getTxnNumber() const {
    return _txnNumber;
}

boost::optional<ReadPreferenceSetting> ClusterClientCursorMock::getReadPreference() const {
    return boost::none;
}

}  // namespace mongo
