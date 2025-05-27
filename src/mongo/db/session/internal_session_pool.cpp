/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/session/internal_session_pool.h"

#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"

#include <iterator>
#include <memory>
#include <mutex>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

const auto serviceDecorator = ServiceContext::declareDecoration<InternalSessionPool>();

auto InternalSessionPool::get(ServiceContext* serviceContext) -> InternalSessionPool* {
    return &serviceDecorator(serviceContext);
}

auto InternalSessionPool::get(OperationContext* opCtx) -> InternalSessionPool* {
    return get(opCtx->getServiceContext());
}

void InternalSessionPool::_reapExpiredSessions(WithLock) {
    auto service = serviceDecorator.owner(this);

    for (auto it = _perUserSessionPool.begin(); it != _perUserSessionPool.end();) {
        auto& userSessionPool = it->second;
        while (!userSessionPool.empty() &&
               userSessionPool.back()._isExpired(service->getFastClockSource()->now())) {
            userSessionPool.pop_back();
        }

        if (userSessionPool.empty()) {
            it = _perUserSessionPool.erase(it, std::next(it));
        } else {
            ++it;
        }
    }
}

boost::optional<InternalSessionPool::Session> InternalSessionPool::_acquireSession(
    SHA256Block userDigest, WithLock) {
    if (!_perUserSessionPool.contains(userDigest)) {
        _perUserSessionPool.try_emplace(userDigest, std::list<InternalSessionPool::Session>{});
    }

    auto& userSessionPool = _perUserSessionPool.at(userDigest);

    if (!userSessionPool.empty()) {
        auto session = std::move(userSessionPool.front());
        userSessionPool.pop_front();

        // Check if most recent session has expired before checking it out.
        auto service = serviceDecorator.owner(this);
        if (!session._isExpired(service->getFastClockSource()->now())) {
            return session;
        }

        // Most recent available session is expired, so all sessions must be expired. Clear
        // list.
        userSessionPool.clear();
        _perUserSessionPool.erase(userDigest);
    }

    return boost::none;
}

InternalSessionPool::Session InternalSessionPool::acquireSystemSession() {
    InternalSessionPool::Session session = [&] {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        const auto& systemSession = makeSystemLogicalSessionId();
        auto session = _acquireSession(systemSession.getUid(), lock);
        return session ? *session : InternalSessionPool::Session(systemSession, TxnNumber(0));
    }();

    LOGV2_DEBUG(5876603,
                2,
                "Acquired standalone internal session for system",
                "lsid"_attr = session.getSessionId(),
                "txnNumber"_attr = session.getTxnNumber());

    return session;
}

InternalSessionPool::Session InternalSessionPool::acquireStandaloneSession(
    OperationContext* opCtx) {
    InternalSessionPool::Session session = [&] {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        const auto& userDigest = getLogicalSessionUserDigestForLoggedInUser(opCtx);
        auto session = _acquireSession(userDigest, lock);
        return session ? *session
                       : InternalSessionPool::Session(makeLogicalSessionId(opCtx), TxnNumber(0));
    }();

    LOGV2_DEBUG(5876600,
                2,
                "Acquired standalone internal session for logged-in user",
                "lsid"_attr = session.getSessionId(),
                "txnNumber"_attr = session.getTxnNumber());

    return session;
}

InternalSessionPool::Session InternalSessionPool::acquireChildSession(
    OperationContext* opCtx, const LogicalSessionId& parentLsid) {
    InternalSessionPool::Session session = [&] {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        auto it = _childSessions.find(parentLsid);
        if (it != _childSessions.end()) {
            auto session = std::move(it->second);
            invariant(parentLsid == getParentSessionId(session.getSessionId()));
            _childSessions.erase(it);
            return session;
        } else {
            auto lsid = makeLogicalSessionIdWithTxnUUID(parentLsid);
            auto session = InternalSessionPool::Session(lsid, TxnNumber(0));
            return session;
        }
    }();

    LOGV2_DEBUG(5876601,
                2,
                "Acquired internal session with parent session",
                "lsid"_attr = session.getSessionId(),
                "txnNumber"_attr = session.getTxnNumber(),
                "parentLsid"_attr = parentLsid);

    return session;
}

void InternalSessionPool::release(Session session) {
    LOGV2_DEBUG(5876602,
                2,
                "Released internal session",
                "lsid"_attr = session.getSessionId(),
                "txnNumber"_attr = session.getTxnNumber());

    ++session._txnNumber;
    const auto& lsid = session.getSessionId();
    if (lsid.getTxnUUID()) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _childSessions.insert({*getParentSessionId(lsid), std::move(session)});
    } else {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (!_perUserSessionPool.contains(lsid.getUid())) {
            _perUserSessionPool.try_emplace(lsid.getUid(),
                                            std::list<InternalSessionPool::Session>{});
        }
        auto& userSessionPool = _perUserSessionPool.at(lsid.getUid());
        auto service = serviceDecorator.owner(this);
        session._lastSeen = service->getFastClockSource()->now();
        userSessionPool.push_front(std::move(session));

        // Clean up session pool by reaping all expired sessions.
        _reapExpiredSessions(lock);
    }
}

std::size_t InternalSessionPool::numSessionsForUser_forTest(SHA256Block userDigest) {
    return _perUserSessionPool[userDigest].size();
}

}  // namespace mongo
