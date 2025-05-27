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

#include "mongo/transport/session.h"

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"

namespace mongo {
namespace transport {

namespace {

AtomicWord<unsigned long long> sessionIdCounter(0);

}  // namespace

Session::Session() : _id(sessionIdCounter.addAndFetch(1)) {}
Session::~Session() {
    if (_inOperation) {
        // Session died while OperationContext was still active.
        // Mark operation completed in SessionManager to resolve counts.
        if (auto sm = _sessionManager.lock()) {
            sm->_completedOperations.fetchAndAddRelaxed(1);
        }
    }
}

void Session::setInOperation(bool state) {
    // On first call, resolve the SessionManager associated with
    // this session and store a weak reference to it on the base Session.
    // This is necessary because if we need access to it in the destructor,
    // we won't be able to refer to the vtable's getTransportLayer().
    // As a bonus, subsequent invocations of setInOpertion() also end up with the
    // stashed copy of SessionManager rather than having to walk the pointer tree.
    auto sm = [this] {
        if (auto smgr = _sessionManager.lock())
            return smgr;

        auto* tl = getTransportLayer();
        if (MONGO_unlikely(!tl))
            return std::shared_ptr<SessionManager>();
        auto smgr = tl->getSharedSessionManager();
        _sessionManager = smgr;
        return smgr;
    }();
    if (MONGO_unlikely(!sm))
        return;

    auto oldState = std::exchange(_inOperation, state);
    if (state) {
        uassert(ErrorCodes::InvalidOptions,
                "Operation started on session already in an active operation",
                !oldState);
        sm->_totalOperations.fetchAndAddRelaxed(1);
    } else if (oldState) {
        sm->_completedOperations.fetchAndAddRelaxed(1);
    }
}

}  // namespace transport
}  // namespace mongo
