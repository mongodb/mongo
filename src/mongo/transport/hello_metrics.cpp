/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/transport/hello_metrics.h"

#include "mongo/logv2/log.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/decorable.h"
#include "mongo/util/testing_proctor.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
// TODO SERVER-84117: Tighten up Session object lifecycle
const auto transportlessHelloMetrics = ServiceContext::declareDecoration<HelloMetrics>();
const auto inExhaustHelloDecoration = transport::Session::declareDecoration<InExhaustHello>();

HelloMetrics* getHelloMetrics(const transport::Session* session) {
    if (!session)
        return nullptr;
    auto tl = session->getTransportLayer();
    if (!tl)
        return nullptr;
    auto sm = tl->getSessionManager();
    if (!sm)
        return nullptr;
    return &sm->helloMetrics;
}

HelloMetrics* getHelloMetrics(const InExhaustHello* decoration) {
    return getHelloMetrics(inExhaustHelloDecoration.owner(decoration));
}
}  // namespace

HelloMetrics* HelloMetrics::get(OperationContext* opCtx) {
    if (auto session = opCtx->getClient()->session()) {
        if (auto transportHelloMetrics = getHelloMetrics(session.get())) {
            return transportHelloMetrics;
        }

        // UnitTests are allowed to perform operations which read/write HelloMetrics
        // even if no TransportLayer/SessionManager is setup.
        // Conversely, a normal server process with a Session
        // MUST have a SessionManager to record stats on.
        massert(8076990,
                "Accessing HelloMetrics on operation not attached to a transport",
                TestingProctor::instance().isEnabled());

        // Fallthrough...
    }

    // Internal worker threads may spawn a Client without a Session to perform background tasks.
    // We can ignore the usage of HelloMetrics on these threads.
    LOGV2_DEBUG(8076991, 3, "Using global HelloMetrics");
    return &transportlessHelloMetrics(opCtx->getServiceContext());
}

size_t HelloMetrics::getNumExhaustIsMaster() const {
    return _exhaustIsMasterConnections.load();
}

void HelloMetrics::incrementNumExhaustIsMaster() {
    _exhaustIsMasterConnections.fetchAndAdd(1);
}

void HelloMetrics::decrementNumExhaustIsMaster() {
    _exhaustIsMasterConnections.fetchAndSubtract(1);
}

size_t HelloMetrics::getNumExhaustHello() const {
    return _exhaustHelloConnections.load();
}

void HelloMetrics::incrementNumExhaustHello() {
    _exhaustHelloConnections.fetchAndAdd(1);
}

void HelloMetrics::decrementNumExhaustHello() {
    _exhaustHelloConnections.fetchAndSubtract(1);
}

size_t HelloMetrics::getNumAwaitingTopologyChanges() const {
    return _connectionsAwaitingTopologyChanges.load();
}

void HelloMetrics::incrementNumAwaitingTopologyChanges() {
    _connectionsAwaitingTopologyChanges.fetchAndAdd(1);
}

void HelloMetrics::decrementNumAwaitingTopologyChanges() {
    _connectionsAwaitingTopologyChanges.fetchAndSubtract(1);
}

void HelloMetrics::resetNumAwaitingTopologyChanges() {
    _connectionsAwaitingTopologyChanges.store(0);
}

void HelloMetrics::resetNumAwaitingTopologyChangesForAllSessionManagers(ServiceContext* svcCtx) {
    if (auto* tlm = svcCtx->getTransportLayerManager()) {
        tlm->forEach([](transport::TransportLayer* tl) {
            if (tl->getSessionManager())
                tl->getSessionManager()->helloMetrics.resetNumAwaitingTopologyChanges();
        });
    }

    if (TestingProctor::instance().isEnabled())
        transportlessHelloMetrics(svcCtx).resetNumAwaitingTopologyChanges();
}

void HelloMetrics::serialize(BSONObjBuilder* builder) const {
    builder->append("exhaustIsMaster"_sd, static_cast<long long>(getNumExhaustIsMaster()));
    builder->append("exhaustHello"_sd, static_cast<long long>(getNumExhaustHello()));
    builder->append("awaitingTopologyChanges"_sd,
                    static_cast<long long>(getNumAwaitingTopologyChanges()));
}

InExhaustHello* InExhaustHello::get(transport::Session* session) {
    auto* ieh = &inExhaustHelloDecoration(session);
    if (!ieh->_sessionManager) {
        // Just in time initialization of _sessionManager weak_ptr.
        if (auto* tl = session->getTransportLayer()) {
            ieh->_sessionManager =
                std::weak_ptr<transport::SessionManager>(tl->getSharedSessionManager());
        }
    }
    return ieh;
}

InExhaustHello::~InExhaustHello() {
    if (!_inExhaustIsMaster && !_inExhaustHello) {
        // Nothing to decrement.
        return;
    }

    // _sessionManager.lock() may return an empty shared_ptr<SessionManager> if any of:
    // 1) _sessionManager was never initialized, meaning there are no stats to back out.
    // 2) _sessionManager was initialized empty, because this is a unit test and not associated with
    // a SessionManager. 3) The underlying SessionManager has already been destructed by its owning
    // TransportLayer. In any of these cases, we don't have to care about backing out stats here.
    invariant(!!_sessionManager);
    if (auto sessionManager = _sessionManager->lock()) {
        if (_inExhaustIsMaster) {
            sessionManager->helloMetrics.decrementNumExhaustIsMaster();
        }
        if (_inExhaustHello) {
            sessionManager->helloMetrics.decrementNumExhaustHello();
        }
    }
}

bool InExhaustHello::getInExhaustIsMaster() const {
    return _inExhaustIsMaster;
}

bool InExhaustHello::getInExhaustHello() const {
    return _inExhaustHello;
}

void InExhaustHello::setInExhaust(bool inExhaust, StringData commandName) {
    const bool isHello = (commandName == "hello"_sd);
    auto* helloMetrics = getHelloMetrics(this);

    // Transition out of exhaust hello if setting inExhaust to false or if
    // the isMaster command is used.
    if (_inExhaustHello && (!inExhaust || !isHello)) {
        helloMetrics->decrementNumExhaustHello();
        _inExhaustHello = false;
    }

    // Transition out of exhaust isMaster if setting inExhaust to false or if
    // the hello command is used.
    if (_inExhaustIsMaster && (!inExhaust || isHello)) {
        helloMetrics->decrementNumExhaustIsMaster();
        _inExhaustIsMaster = false;
    }

    if (inExhaust) {
        if (isHello && !_inExhaustHello) {
            helloMetrics->incrementNumExhaustHello();
            _inExhaustHello = inExhaust;
        } else if (!isHello && !_inExhaustIsMaster) {
            helloMetrics->incrementNumExhaustIsMaster();
            _inExhaustIsMaster = inExhaust;
        }
    }
}

}  // namespace mongo
