/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_client_cursor_mock.h"

#include "mongo/util/assert_util.h"

namespace mongo {

ClusterClientCursorMock::ClusterClientCursorMock(stdx::function<void(void)> killCallback)
    : _killCallback(std::move(killCallback)) {}

ClusterClientCursorMock::~ClusterClientCursorMock() {
    invariant(_exhausted || _killed);
}

StatusWith<boost::optional<BSONObj>> ClusterClientCursorMock::next() {
    invariant(!_killed);

    if (_resultsQueue.empty()) {
        _exhausted = true;
        return {boost::none};
    }

    auto out = _resultsQueue.front();
    _resultsQueue.pop();

    if (!out.isOK()) {
        return out.getStatus();
    }

    ++_numReturnedSoFar;
    return boost::optional<BSONObj>(out.getValue());
}

long long ClusterClientCursorMock::getNumReturnedSoFar() const {
    return _numReturnedSoFar;
}

void ClusterClientCursorMock::kill() {
    _killed = true;
    if (_killCallback) {
        _killCallback();
    }
}

bool ClusterClientCursorMock::isTailable() const {
    return false;
}

void ClusterClientCursorMock::queueResult(const BSONObj& obj) {
    _resultsQueue.push({obj});
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

}  // namespace mongo
