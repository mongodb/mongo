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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/service_context.h"

#include <list>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/default_baton.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/system_tick_source.h"
#include <iostream>

namespace mongo {
namespace {

using ConstructorActionList = std::list<ServiceContext::ConstructorDestructorActions>;

ServiceContext* globalServiceContext = nullptr;

AtomicWord<int> _numCurrentOps{0};

}  // namespace

bool hasGlobalServiceContext() {
    return globalServiceContext;
}

ServiceContext* getGlobalServiceContext() {
    fassert(17508, globalServiceContext);
    return globalServiceContext;
}

void setGlobalServiceContext(ServiceContext::UniqueServiceContext&& serviceContext) {
    if (globalServiceContext) {
        // Make sure that calling getGlobalServiceContext() during the destructor results in
        // nullptr. Decorations might try and do this.
        ServiceContext::UniqueServiceContext oldServiceContext{globalServiceContext};
        globalServiceContext = nullptr;
    }

    globalServiceContext = serviceContext.release();
}

bool _supportsDocLocking = false;

bool supportsDocLocking() {
    return _supportsDocLocking;
}

ServiceContext::ServiceContext()
    : _tickSource(std::make_unique<SystemTickSource>()),
      _fastClockSource(std::make_unique<SystemClockSource>()),
      _preciseClockSource(std::make_unique<SystemClockSource>()) {}

ServiceContext::~ServiceContext() {
    stdx::lock_guard<Latch> lk(_mutex);
    for (const auto& client : _clients) {
        severe() << "Client " << client->desc() << " still exists while destroying ServiceContext@"
                 << static_cast<void*>(this);
    }
    invariant(_clients.empty());
}

namespace {

//
// These onDestroy and onCreate functions are utilities for correctly executing supplemental
// constructor and destructor methods for the ServiceContext, Client and OperationContext types.
//
// Note that destructors run in reverse order of constructors, and that failed construction
// leads to corresponding destructors to run, similarly to how member variable construction and
// destruction behave.
//

template <typename T, typename ObserversIterator>
void onDestroy(T* object,
               const ObserversIterator& observerBegin,
               const ObserversIterator& observerEnd) {
    try {
        auto observer = observerEnd;
        while (observer != observerBegin) {
            --observer;
            observer->onDestroy(object);
        }
    } catch (...) {
        std::terminate();
    }
}
template <typename T, typename ObserversContainer>
void onDestroy(T* object, const ObserversContainer& observers) {
    onDestroy(object, observers.cbegin(), observers.cend());
}

template <typename T, typename ObserversIterator>
void onCreate(T* object,
              const ObserversIterator& observerBegin,
              const ObserversIterator& observerEnd) {
    auto observer = observerBegin;
    try {
        for (; observer != observerEnd; ++observer) {
            observer->onCreate(object);
        }
    } catch (...) {
        onDestroy(object, observerBegin, observer);
        throw;
    }
}

template <typename T, typename ObserversContainer>
void onCreate(T* object, const ObserversContainer& observers) {
    onCreate(object, observers.cbegin(), observers.cend());
}

}  // namespace

ServiceContext::UniqueClient ServiceContext::makeClient(std::string desc,
                                                        transport::SessionHandle session) {
    std::unique_ptr<Client> client(new Client(std::move(desc), this, std::move(session)));
    onCreate(client.get(), _clientObservers);
    {
        stdx::lock_guard<Latch> lk(_mutex);
        invariant(_clients.insert(client.get()).second);
    }
    return UniqueClient(client.release());
}

void ServiceContext::setPeriodicRunner(std::unique_ptr<PeriodicRunner> runner) {
    invariant(!_runner);
    _runner = std::move(runner);
}

PeriodicRunner* ServiceContext::getPeriodicRunner() const {
    return _runner.get();
}

transport::TransportLayer* ServiceContext::getTransportLayer() const {
    return _transportLayer.get();
}

ServiceEntryPoint* ServiceContext::getServiceEntryPoint() const {
    return _serviceEntryPoint.get();
}

transport::ServiceExecutor* ServiceContext::getServiceExecutor() const {
    return _serviceExecutor.get();
}

void ServiceContext::setStorageEngine(std::unique_ptr<StorageEngine> engine) {
    invariant(engine);
    invariant(!_storageEngine);
    _storageEngine = std::move(engine);
}

void ServiceContext::setOpObserver(std::unique_ptr<OpObserver> opObserver) {
    _opObserver = std::move(opObserver);
}

void ServiceContext::setTickSource(std::unique_ptr<TickSource> newSource) {
    _tickSource = std::move(newSource);
}

void ServiceContext::setFastClockSource(std::unique_ptr<ClockSource> newSource) {
    _fastClockSource = std::move(newSource);
}

void ServiceContext::setPreciseClockSource(std::unique_ptr<ClockSource> newSource) {
    _preciseClockSource = std::move(newSource);
}

void ServiceContext::setServiceEntryPoint(std::unique_ptr<ServiceEntryPoint> sep) {
    _serviceEntryPoint = std::move(sep);
}

void ServiceContext::setTransportLayer(std::unique_ptr<transport::TransportLayer> tl) {
    _transportLayer = std::move(tl);
}

void ServiceContext::setServiceExecutor(std::unique_ptr<transport::ServiceExecutor> exec) {
    _serviceExecutor = std::move(exec);
}

void ServiceContext::ClientDeleter::operator()(Client* client) const {
    ServiceContext* const service = client->getServiceContext();
    {
        stdx::lock_guard<Latch> lk(service->_mutex);
        invariant(service->_clients.erase(client));
    }
    onDestroy(client, service->_clientObservers);
    delete client;
}

ServiceContext::UniqueOperationContext ServiceContext::makeOperationContext(Client* client) {
    auto opCtx = std::make_unique<OperationContext>(client, _nextOpId.fetchAndAdd(1));
    if (client->session()) {
        _numCurrentOps.addAndFetch(1);
    }

    onCreate(opCtx.get(), _clientObservers);
    if (!opCtx->lockState()) {
        opCtx->setLockState(std::make_unique<LockerNoop>());
    }
    if (!opCtx->recoveryUnit()) {
        opCtx->setRecoveryUnit(std::make_unique<RecoveryUnitNoop>(),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }
    // The baton must be attached before attaching to a client
    if (_transportLayer) {
        _transportLayer->makeBaton(opCtx.get());
    } else {
        makeBaton(opCtx.get());
    }
    {
        stdx::lock_guard<Client> lk(*client);
        client->setOperationContext(opCtx.get());
    }
    return UniqueOperationContext(opCtx.release());
};

void ServiceContext::OperationContextDeleter::operator()(OperationContext* opCtx) const {
    auto client = opCtx->getClient();
    if (client->session()) {
        _numCurrentOps.subtractAndFetch(1);
    }
    auto service = client->getServiceContext();
    {
        stdx::lock_guard<Client> lk(*client);
        client->resetOperationContext();
    }
    opCtx->getBaton()->detach();

    onDestroy(opCtx, service->_clientObservers);
    delete opCtx;
}

void ServiceContext::registerClientObserver(std::unique_ptr<ClientObserver> observer) {
    _clientObservers.emplace_back(std::move(observer));
}

ServiceContext::LockedClientsCursor::LockedClientsCursor(ServiceContext* service)
    : _lock(service->_mutex), _curr(service->_clients.cbegin()), _end(service->_clients.cend()) {}

Client* ServiceContext::LockedClientsCursor::next() {
    if (_curr == _end)
        return nullptr;
    Client* result = *_curr;
    ++_curr;
    return result;
}

void ServiceContext::setKillAllOperations() {
    stdx::lock_guard<Latch> clientLock(_mutex);

    // Ensure that all newly created operation contexts will immediately be in the interrupted state
    _globalKill.store(true);

    // Interrupt all active operations
    for (auto&& client : _clients) {
        stdx::lock_guard<Client> lk(*client);
        auto opCtxToKill = client->getOperationContext();
        if (opCtxToKill) {
            killOperation(lk, opCtxToKill, ErrorCodes::InterruptedAtShutdown);
        }
    }

    // Notify any listeners who need to reach to the server shutting down
    for (const auto listener : _killOpListeners) {
        try {
            listener->interruptAll();
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::killOperation(WithLock, OperationContext* opCtx, ErrorCodes::Error killCode) {
    opCtx->markKilled(killCode);

    for (const auto listener : _killOpListeners) {
        try {
            listener->interrupt(opCtx->getOpID());
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::unsetKillAllOperations() {
    _globalKill.store(false);
}

void ServiceContext::registerKillOpListener(KillOpListenerInterface* listener) {
    stdx::lock_guard<Latch> clientLock(_mutex);
    _killOpListeners.push_back(listener);
}

void ServiceContext::waitForStartupComplete() {
    stdx::unique_lock<Latch> lk(_mutex);
    _startupCompleteCondVar.wait(lk, [this] { return _startupComplete; });
}

void ServiceContext::notifyStartupComplete() {
    stdx::unique_lock<Latch> lk(_mutex);
    _startupComplete = true;
    lk.unlock();
    _startupCompleteCondVar.notify_all();
}

int ServiceContext::getActiveClientOperations() {
    return _numCurrentOps.load();
}

namespace {

/**
 * Accessor function to get the global list of ServiceContext constructor and destructor
 * functions.
 */
ConstructorActionList& registeredConstructorActions() {
    static ConstructorActionList cal;
    return cal;
}

}  // namespace

ServiceContext::ConstructorActionRegisterer::ConstructorActionRegisterer(
    std::string name, ConstructorAction constructor, DestructorAction destructor)
    : ConstructorActionRegisterer(
          std::move(name), {}, std::move(constructor), std::move(destructor)) {}

ServiceContext::ConstructorActionRegisterer::ConstructorActionRegisterer(
    std::string name,
    std::vector<std::string> prereqs,
    ConstructorAction constructor,
    DestructorAction destructor) {
    if (!destructor)
        destructor = [](ServiceContext*) {};
    _registerer.emplace(std::move(name),
                        [this, constructor, destructor](InitializerContext* context) {
                            _iter = registeredConstructorActions().emplace(
                                registeredConstructorActions().end(),
                                std::move(constructor),
                                std::move(destructor));
                            return Status::OK();
                        },
                        [this](DeinitializerContext* context) {
                            registeredConstructorActions().erase(_iter);
                            return Status::OK();
                        },
                        std::move(prereqs));
}

ServiceContext::UniqueServiceContext ServiceContext::make() {
    auto service = std::make_unique<ServiceContext>();
    onCreate(service.get(), registeredConstructorActions());
    return UniqueServiceContext{service.release()};
}

void ServiceContext::ServiceContextDeleter::operator()(ServiceContext* service) const {
    onDestroy(service, registeredConstructorActions());
    delete service;
}

BatonHandle ServiceContext::makeBaton(OperationContext* opCtx) const {
    invariant(!opCtx->getBaton());

    auto baton = std::make_shared<DefaultBaton>(opCtx);
    opCtx->setBaton(baton);

    return baton;
}

}  // namespace mongo
