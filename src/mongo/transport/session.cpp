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

#include "mongo/platform/atomic.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"

namespace mongo {
namespace transport {

namespace {

AtomicWord<unsigned long long> sessionIdCounter(0);

}  // namespace

Session::Session() : _id(sessionIdCounter.addAndFetch(1)) {}
Session::~Session() {
    if (_opCounters && _inOperation) {
        _opCounters->completed.fetchAndAddRelaxed(1);
    }
}

void Session::setInOperation(bool state) {
    if (MONGO_unlikely(!_opCounters)) {
        // We should only take this path once for each connection in production, so we are opting
        // for readability over performance here.
        auto tl = getTransportLayer();
        if (MONGO_unlikely(!tl))
            return;

        auto sm = tl->getSessionManager();
        if (MONGO_unlikely(!sm))
            return;

        _opCounters = sm->getOpCounters();
    }

    auto oldState = std::exchange(_inOperation, state);
    if (state) {
        uassert(ErrorCodes::InvalidOptions,
                "Operation started on session already in an active operation",
                !oldState);
        _opCounters->total.fetchAndAddRelaxed(1);
    } else if (oldState) {
        _opCounters->completed.fetchAndAddRelaxed(1);
    }
}

}  // namespace transport
}  // namespace mongo
