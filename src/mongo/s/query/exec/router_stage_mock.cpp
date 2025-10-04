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


#include "mongo/s/query/exec/router_stage_mock.h"

#include "mongo/base/error_codes.h"

#include <boost/move/utility_core.hpp>
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
