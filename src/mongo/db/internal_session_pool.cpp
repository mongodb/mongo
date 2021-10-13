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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/internal_session_pool.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/logv2/log.h"

namespace mongo {

const auto serviceDecorator = ServiceContext::declareDecoration<InternalSessionPool>();

auto InternalSessionPool::get(ServiceContext* serviceContext) -> InternalSessionPool* {
    return &serviceDecorator(serviceContext);
}

auto InternalSessionPool::get(OperationContext* opCtx) -> InternalSessionPool* {
    return get(opCtx->getServiceContext());
}

InternalSessionPool::Session InternalSessionPool::acquire(OperationContext* opCtx) {
    const InternalSessionPool::Session session = [&] {
        stdx::lock_guard<Latch> lock(_mutex);

        if (!_nonChildSessions.empty()) {
            auto session = std::move(_nonChildSessions.top());
            _nonChildSessions.pop();
            return session;
        } else {
            auto lsid = makeSystemLogicalSessionId();
            auto session = InternalSessionPool::Session(lsid, TxnNumber(0));

            auto lsc = LogicalSessionCache::get(opCtx->getServiceContext());
            uassertStatusOK(lsc->vivify(opCtx, lsid));
            return session;
        }
    }();

    LOGV2_DEBUG(5876600,
                2,
                "Acquired internal session",
                "lsid"_attr = session.getSessionId(),
                "txnNumber"_attr = session.getTxnNumber());

    return session;
}

InternalSessionPool::Session InternalSessionPool::acquire(OperationContext* opCtx,
                                                          const LogicalSessionId& parentLsid) {
    const InternalSessionPool::Session session = [&] {
        stdx::lock_guard<Latch> lock(_mutex);

        auto it = _childSessions.find(parentLsid);
        if (it != _childSessions.end()) {
            auto session = std::move(it->second);
            _childSessions.erase(it);
            return session;
        } else {
            auto lsid = LogicalSessionId{parentLsid.getId(), parentLsid.getUid()};

            lsid.getInternalSessionFields().setTxnUUID(UUID::gen());

            auto session = InternalSessionPool::Session(lsid, TxnNumber(0));

            auto lsc = LogicalSessionCache::get(opCtx->getServiceContext());
            uassertStatusOK(lsc->vivify(opCtx, lsid));
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

    session.setTxnNumber(session.getTxnNumber() + 1);
    if (session.getSessionId().getTxnUUID()) {
        auto lsid = session.getSessionId();
        stdx::lock_guard<Latch> lock(_mutex);
        _childSessions.insert({std::move(lsid), std::move(session)});
    } else {
        stdx::lock_guard<Latch> lock(_mutex);
        _nonChildSessions.push(std::move(session));
    }
}

}  // namespace mongo
