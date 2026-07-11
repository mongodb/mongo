// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/query/exec/router_stage_mock.h"

#include "mongo/base/error_codes.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

void RouterStageMock::queueResult(ClusterQueryResult&& result) {
    _resultsQueue.push({std::move(result)});
}

void RouterStageMock::queueError(Status status) {
    _resultsQueue.push({status});
}

void RouterStageMock::queueEOF() {
    _resultsQueue.push({ClusterQueryResult()});
}

void RouterStageMock::markRemotesExhausted() {
    _remotesExhausted = true;
}

StatusWith<ClusterQueryResult> RouterStageMock::next() {
    if (_resultsQueue.empty()) {
        return {ClusterQueryResult()};
    }

    auto out = std::move(_resultsQueue.front());
    _resultsQueue.pop();
    return out;
}

void RouterStageMock::kill(OperationContext* opCtx) {
    // No child to kill.
}

bool RouterStageMock::remotesExhausted() const {
    return _remotesExhausted;
}

bool RouterStageMock::isEOF() const {
    return _resultsQueue.empty() && _remotesExhausted;
}

Status RouterStageMock::doSetAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    _awaitDataTimeout = awaitDataTimeout;
    return Status::OK();
}

StatusWith<Milliseconds> RouterStageMock::getAwaitDataTimeout() {
    if (!_awaitDataTimeout) {
        return Status(ErrorCodes::BadValue, "no awaitData timeout set");
    }

    return *_awaitDataTimeout;
}

}  // namespace mongo
