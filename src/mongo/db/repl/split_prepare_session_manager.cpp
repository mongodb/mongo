// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/split_prepare_session_manager.h"

#include "mongo/util/assert_util.h"

#include <mutex>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>

namespace mongo {
namespace repl {

SplitPrepareSessionManager::SplitPrepareSessionManager(InternalSessionPool* sessionPool)
    : _sessionPool(sessionPool) {}

const std::vector<SplitSessionInfo>& SplitPrepareSessionManager::splitSession(
    const LogicalSessionId& sessionId,
    TxnNumber txnNumber,
    const std::vector<uint32_t>& requesterIds) {

    auto numSplits = requesterIds.size();
    invariant(numSplits > 0);
    std::lock_guard<std::mutex> lk(_mutex);

    auto [it, succ] =
        _splitSessionMap.try_emplace(sessionId, txnNumber, std::vector<SplitSessionInfo>());

    // The session must not be split before.
    invariant(succ);

    auto& sessionInfos = it->second.second;
    sessionInfos.reserve(numSplits);

    for (auto reqId : requesterIds) {
        sessionInfos.emplace_back(_sessionPool->acquireSystemSession(), reqId);
    }

    return sessionInfos;
}

boost::optional<const std::vector<SplitSessionInfo>&> SplitPrepareSessionManager::getSplitSessions(
    const LogicalSessionId& sessionId, TxnNumber txnNumber) const {
    std::lock_guard<std::mutex> lk(_mutex);

    auto it = _splitSessionMap.find(sessionId);
    if (it == _splitSessionMap.end()) {
        return boost::none;
    }

    // The txnNumber must not change after the session was split.
    invariant(txnNumber == it->second.first);

    return it->second.second;
}

bool SplitPrepareSessionManager::isSessionSplit(const LogicalSessionId& sessionId,
                                                TxnNumber txnNumber) const {
    std::lock_guard<std::mutex> lk(_mutex);

    auto it = _splitSessionMap.find(sessionId);
    if (it == _splitSessionMap.end()) {
        return false;
    }

    // The txnNumber must not change after the session was split.
    invariant(txnNumber == it->second.first);

    return true;
}

void SplitPrepareSessionManager::releaseSplitSessions(const LogicalSessionId& sessionId,
                                                      TxnNumber txnNumber) {
    std::lock_guard<std::mutex> lk(_mutex);

    auto it = _splitSessionMap.find(sessionId);

    // The session must already be split and tracked.
    invariant(it != _splitSessionMap.end());

    // The txnNumber must not change after the session was split.
    invariant(txnNumber == it->second.first);

    auto& sessionInfos = it->second.second;
    for (const auto& sessInfo : sessionInfos) {
        _sessionPool->release(sessInfo.session);
    }

    _splitSessionMap.erase(it);
}

void SplitPrepareSessionManager::releaseAllSplitSessions() {
    std::lock_guard<std::mutex> lk(_mutex);

    for (const auto& entry : _splitSessionMap) {
        for (const auto& sessInfo : entry.second.second) {
            _sessionPool->release(sessInfo.session);
        }
    }

    _splitSessionMap.clear();
}

}  // namespace repl
}  // namespace mongo
