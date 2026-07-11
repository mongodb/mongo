// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/query/exec/cluster_client_cursor_mock.h"

#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

ClusterClientCursorMock::ClusterClientCursorMock(boost::optional<LogicalSessionId> lsid,
                                                 boost::optional<TxnNumber> txnNumber,
                                                 std::function<void(void)> killCallback,
                                                 bool isChangeStreamCursor)
    : _killCallback(std::move(killCallback)),
      _lsid(lsid),
      _txnNumber(txnNumber),
      _isChangeStreamCursor(isChangeStreamCursor) {}

ClusterClientCursorMock::~ClusterClientCursorMock() {
    invariant(_remotesExhausted || _killed);
}

StatusWith<ClusterQueryResult> ClusterClientCursorMock::next() {
    tassert(11052322, "Expected cursor not killed", !_killed);

    if (_resultsQueue.empty()) {
        _remotesExhausted = true;
        return {ClusterQueryResult()};
    }

    auto out = std::move(_resultsQueue.front());
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

const PrivilegeVector& ClusterClientCursorMock::getOriginatingPrivileges() const& {
    return _originatingPrivileges;
}

bool ClusterClientCursorMock::partialResultsReturned() const {
    MONGO_UNREACHABLE_TASSERT(11052359);
}

std::size_t ClusterClientCursorMock::getNumRemotes() const {
    MONGO_UNREACHABLE_TASSERT(11052360);
}

BSONObj ClusterClientCursorMock::getPostBatchResumeToken() const {
    MONGO_UNREACHABLE_TASSERT(11052361);
}

long long ClusterClientCursorMock::getNumReturnedSoFar() const {
    return _numReturnedSoFar;
}

Date_t ClusterClientCursorMock::getCreatedDate() const {
    return _createdDate;
}

Date_t ClusterClientCursorMock::getLastUseDate() const {
    return _lastUseDate;
}

bool ClusterClientCursorMock::isChangeStreamCursor() const {
    return _isChangeStreamCursor;
}

bool ClusterClientCursorMock::usesChangeStreamV2ShardTargeting() const {
    return false;
}

void ClusterClientCursorMock::setLastUseDate(Date_t now) {
    _lastUseDate = std::move(now);
}

boost::optional<uint32_t> ClusterClientCursorMock::getPlanCacheShapeHash() const {
    return boost::none;
}

boost::optional<query_shape::QueryShapeHash> ClusterClientCursorMock::getQueryShapeHash() const {
    return boost::none;
}

boost::optional<std::size_t> ClusterClientCursorMock::getQueryStatsKeyHash() const {
    return boost::none;
}

void ClusterClientCursorMock::kill(OperationContext* opCtx) {
    _killed = true;
    if (_killCallback) {
        _killCallback();
    }
}

Status ClusterClientCursorMock::releaseMemory() {
    return Status::OK();
}

bool ClusterClientCursorMock::isTailable() const {
    return false;
}

bool ClusterClientCursorMock::isTailableAndAwaitData() const {
    return false;
}

void ClusterClientCursorMock::queueResult(ClusterQueryResult&& result) {
    _resultsQueue.push({std::move(result)});
}

bool ClusterClientCursorMock::remotesExhausted() const {
    return _remotesExhausted;
}

bool ClusterClientCursorMock::isEOF() const {
    return _resultsQueue.empty() && _remotesExhausted;
}

bool ClusterClientCursorMock::hasBeenKilled() const {
    return _killed;
}

void ClusterClientCursorMock::queueError(Status status) {
    _resultsQueue.push({status});
}

Status ClusterClientCursorMock::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    MONGO_UNREACHABLE_TASSERT(11052362);
}

boost::optional<LogicalSessionId> ClusterClientCursorMock::getLsid() const {
    return _lsid;
}

boost::optional<TxnNumber> ClusterClientCursorMock::getTxnNumber() const {
    return _txnNumber;
}

void ClusterClientCursorMock::setAPIParameters(APIParameters& apiParameters) {
    _apiParameters = apiParameters;
}

APIParameters ClusterClientCursorMock::getAPIParameters() const {
    return _apiParameters;
}

boost::optional<ReadPreferenceSetting> ClusterClientCursorMock::getReadPreference() const {
    return boost::none;
}

boost::optional<repl::ReadConcernArgs> ClusterClientCursorMock::getReadConcern() const {
    return boost::none;
}

bool ClusterClientCursorMock::shouldOmitDiagnosticInformation() const {
    return false;
}

std::unique_ptr<query_stats::Key> ClusterClientCursorMock::takeKey() {
    return nullptr;
}

}  // namespace mongo
