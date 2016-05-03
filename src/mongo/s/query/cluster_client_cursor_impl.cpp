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

#include "mongo/s/query/cluster_client_cursor_impl.h"

#include "mongo/s/query/router_stage_limit.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/s/query/router_stage_mock.h"
#include "mongo/s/query/router_stage_remove_sortkey.h"
#include "mongo/s/query/router_stage_skip.h"
#include "mongo/stdx/memory.h"

namespace mongo {

ClusterClientCursorGuard::ClusterClientCursorGuard(std::unique_ptr<ClusterClientCursor> ccc)
    : _ccc(std::move(ccc)) {}

ClusterClientCursorGuard::~ClusterClientCursorGuard() {
    if (_ccc && !_ccc->remotesExhausted()) {
        _ccc->kill();
    }
}

ClusterClientCursor* ClusterClientCursorGuard::operator->() {
    return _ccc.get();
}

std::unique_ptr<ClusterClientCursor> ClusterClientCursorGuard::releaseCursor() {
    return std::move(_ccc);
}

ClusterClientCursorGuard ClusterClientCursorImpl::make(executor::TaskExecutor* executor,
                                                       ClusterClientCursorParams&& params) {
    std::unique_ptr<ClusterClientCursor> cursor(
        new ClusterClientCursorImpl(executor, std::move(params)));
    return ClusterClientCursorGuard(std::move(cursor));
}

ClusterClientCursorImpl::ClusterClientCursorImpl(executor::TaskExecutor* executor,
                                                 ClusterClientCursorParams&& params)
    : _isTailable(params.isTailable), _root(buildMergerPlan(executor, std::move(params))) {}

ClusterClientCursorImpl::ClusterClientCursorImpl(std::unique_ptr<RouterStageMock> root)
    : _root(std::move(root)) {}

StatusWith<boost::optional<BSONObj>> ClusterClientCursorImpl::next() {
    // First return stashed results, if there are any.
    if (!_stash.empty()) {
        BSONObj front = std::move(_stash.front());
        _stash.pop();
        ++_numReturnedSoFar;
        return {front};
    }

    auto next = _root->next();
    if (next.isOK() && next.getValue()) {
        ++_numReturnedSoFar;
    }
    return next;
}

void ClusterClientCursorImpl::kill() {
    _root->kill();
}

bool ClusterClientCursorImpl::isTailable() const {
    return _isTailable;
}

long long ClusterClientCursorImpl::getNumReturnedSoFar() const {
    return _numReturnedSoFar;
}

void ClusterClientCursorImpl::queueResult(const BSONObj& obj) {
    invariant(obj.isOwned());
    _stash.push(obj);
}

bool ClusterClientCursorImpl::remotesExhausted() {
    return _root->remotesExhausted();
}

Status ClusterClientCursorImpl::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return _root->setAwaitDataTimeout(awaitDataTimeout);
}

std::unique_ptr<RouterExecStage> ClusterClientCursorImpl::buildMergerPlan(
    executor::TaskExecutor* executor, ClusterClientCursorParams&& params) {
    const auto skip = params.skip;
    const auto limit = params.limit;
    const bool hasSort = !params.sort.isEmpty();

    // The first stage is always the one which merges from the remotes.
    std::unique_ptr<RouterExecStage> root =
        stdx::make_unique<RouterStageMerge>(executor, std::move(params));

    if (skip) {
        root = stdx::make_unique<RouterStageSkip>(std::move(root), *skip);
    }

    if (limit) {
        root = stdx::make_unique<RouterStageLimit>(std::move(root), *limit);
    }

    if (hasSort) {
        root = stdx::make_unique<RouterStageRemoveSortKey>(std::move(root));
    }

    return root;
}

}  // namespace mongo
