// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
using namespace std::literals::string_view_literals;
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

HelloMetrics& HelloMetrics::operator+=(const HelloMetrics& other) {
    _connectionsAwaitingTopologyChanges.fetchAndAddRelaxed(
        other._connectionsAwaitingTopologyChanges.load());
    _exhaustIsMasterConnections.fetchAndAddRelaxed(other._exhaustIsMasterConnections.load());
    _exhaustHelloConnections.fetchAndAddRelaxed(other._exhaustHelloConnections.load());
    return *this;
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
    builder->append("exhaustIsMaster"sv, static_cast<long long>(getNumExhaustIsMaster()));
    builder->append("exhaustHello"sv, static_cast<long long>(getNumExhaustHello()));
    builder->append("awaitingTopologyChanges"sv,
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

void InExhaustHello::transitionOutOfInExhaustHello(HelloMetrics* helloMetrics) {
    if (!_inExhaustHello)
        return;
    helloMetrics->decrementNumExhaustHello();
    _inExhaustHello = false;
}

void InExhaustHello::transitionOutOfInExhaustIsMaster(HelloMetrics* helloMetrics) {
    if (!_inExhaustIsMaster)
        return;
    helloMetrics->decrementNumExhaustIsMaster();
    _inExhaustIsMaster = false;
}

void InExhaustHello::setInExhaust(Command command) {
    auto* helloMetrics = getHelloMetrics(this);
    if (command == Command::kHello) {
        transitionOutOfInExhaustIsMaster(helloMetrics);
        if (!_inExhaustHello) {
            helloMetrics->incrementNumExhaustHello();
            _inExhaustHello = true;
        }
    } else {
        invariant(command == Command::kIsMaster);
        transitionOutOfInExhaustHello(helloMetrics);
        if (!_inExhaustIsMaster) {
            helloMetrics->incrementNumExhaustIsMaster();
            _inExhaustIsMaster = true;
        }
    }
}

void InExhaustHello::resetInExhaust() {
    auto* helloMetrics = getHelloMetrics(this);
    transitionOutOfInExhaustHello(helloMetrics);
    transitionOutOfInExhaustIsMaster(helloMetrics);
}

}  // namespace mongo
