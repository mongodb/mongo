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


#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/db/client.h"
#include "mongo/db/default_baton.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/system_tick_source.h"

#include <exception>
#include <list>
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

ServiceContext* globalServiceContext = nullptr;

}  // namespace

ClientLock::ClientLock(Client* client) : service_context_detail::ObjectLock<Client>(client) {}

bool hasGlobalServiceContext() {
    return globalServiceContext;
}

ServiceContext* getGlobalServiceContext() {
    fassert(17508, globalServiceContext);
    return globalServiceContext;
}

ServiceContext* getCurrentServiceContext() {
    auto client = Client::getCurrent();
    if (client) {
        return client->getServiceContext();
    }

    return nullptr;
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

logv2::LogService toLogService(Service* service) {
    return toLogService(service ? service->role() : ClusterRole::None);
}

/**
 * The global clusterRole determines which services are initialized.
 * If no role is set, then ShardServer is assumed, so there's always
 * at least one Service created.
 */
struct ServiceContext::ServiceSet {
public:
    explicit ServiceSet(ServiceContext* sc) {
        auto role = serverGlobalParams.clusterRole;
        if (!role.has(ClusterRole::RouterServer))
            role += ClusterRole::ShardServer;
        if (role.has(ClusterRole::RouterServer))
            _router = Service::make(sc, ClusterRole::RouterServer);
        if (role.has(ClusterRole::ShardServer))
            _shard = Service::make(sc, ClusterRole::ShardServer);
    }

    /** The `role` here must be ShardServer or RouterServer exactly. */
    Service* getService(ClusterRole role) {
        if (role.hasExclusively(ClusterRole::ShardServer))
            return _shard.get();
        if (role.hasExclusively(ClusterRole::RouterServer))
            return _router.get();
        MONGO_UNREACHABLE;
    }

private:
    Service::UniqueService _shard;
    Service::UniqueService _router;
};

Service::~Service() = default;
Service::Service(ServiceContext* sc, ClusterRole role) : _sc{sc}, _role{role} {}

ServiceContext::ServiceContext(std::unique_ptr<ClockSource> fastClockSource,
                               std::unique_ptr<ClockSource> preciseClockSource,
                               std::unique_ptr<TickSource> tickSource)
    : _tickSource(std::move(tickSource)),
      _fastClockSource(std::move(fastClockSource)),
      _preciseClockSource(std::move(preciseClockSource)),
      _serviceSet(std::make_unique<ServiceSet>(this)) {
    ObservableMutexRegistry::get().add("ServiceContext::_mutex", _mutex);
}


ServiceContext::~ServiceContext() {
    stdx::lock_guard lk(_mutex);
    for (const auto& [client, _] : _clients) {
        LOGV2_ERROR(23828,
                    "Non-empty client list when destroying service context",
                    "client"_attr = client->desc(),
                    "serviceContext"_attr = reinterpret_cast<uint64_t>(this));
    }
    invariant(_clients.empty());
}

Service* ServiceContext::getService(ClusterRole role) const {
    return _serviceSet->getService(role);
}

Service* ServiceContext::getService() const {
    for (auto role : {ClusterRole::ShardServer, ClusterRole::RouterServer})
        if (auto p = getService(role))
            return p;
    MONGO_UNREACHABLE;
}

void Service::setServiceEntryPoint(std::unique_ptr<ServiceEntryPoint> sep) {
    auto old = _serviceEntryPoint.swap(std::move(sep));
    invariant(!old);
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

ServiceContext::UniqueClient ServiceContext::makeClientForService(
    std::string desc,
    std::shared_ptr<transport::Session> session,
    ClientOperationKillableByStepdown killable,
    Service* service) {
    std::unique_ptr<Client> client(
        new Client(std::move(desc), service, std::move(session), killable));
    onCreate(client.get(), _clientObservers);
    auto entry = _clientsList.add(client.get());
    {
        stdx::lock_guard lk(_mutex);
        invariant(_clients.insert({client.get(), entry}).second);
    }
    return UniqueClient(client.release());
}

void ServiceContext::setPeriodicRunner(std::unique_ptr<PeriodicRunner> runner) {
    auto old = _runner.swap(std::move(runner));
    invariant(!old);
}

PeriodicRunner* ServiceContext::getPeriodicRunner() const {
    return _runner.get();
}

transport::TransportLayerManager* ServiceContext::getTransportLayerManager() const {
    return _transportLayerManager.get();
}

void ServiceContext::setStorageEngine(std::unique_ptr<StorageEngine> engine) {
    invariant(engine);
    invariant(!_storageEngine);
    _storageEngine = std::move(engine);
}

void ServiceContext::setOpObserver(std::unique_ptr<OpObserver> opObserver) {
    auto old = _opObserver.swap(std::move(opObserver));
    invariant(!old);
}

void ServiceContext::resetOpObserver_forTest(std::unique_ptr<OpObserver> opObserver) {
    _opObserver = std::move(opObserver);
}

void ServiceContext::setTransportLayerManager(
    std::unique_ptr<transport::TransportLayerManager> tl) {
    auto old = _transportLayerManager.swap(std::move(tl));
    invariant(!old);
}

void ServiceContext::ClientDeleter::operator()(Client* client) const {
    ServiceContext* const svcCtx = client->getServiceContext();
    OperationIdManager::get(svcCtx).eraseClientFromMap(client);
    auto entry = [&] {
        stdx::lock_guard lk(svcCtx->_mutex);
        auto it = svcCtx->_clients.find(client);
        invariant(it != svcCtx->_clients.end(), "Cannot find client in the list of clients!");
        auto entry = it->second;
        svcCtx->_clients.erase(it);
        return entry;
    }();
    svcCtx->_clientsList.remove(entry);
    onDestroy(client, svcCtx->_clientObservers);
    delete client;
}

ServiceContext::UniqueOperationContext ServiceContext::makeOperationContext(Client* client) {
    auto opCtx = std::make_unique<OperationContext>(
        client, OperationIdManager::get(this).issueForClient(client));

    // If there's an egress transport layer and it can produce a
    // nonnull `BatonHandle`, let it do so. Otherwise, we'll use a
    // `DefaultBaton` as a fallback.
    opCtx->setBaton([&]() -> BatonHandle {
        if (_transportLayerManager)
            if (auto baton =
                    _transportLayerManager->getDefaultEgressLayer()->makeBaton(opCtx.get()))
                return baton;
        return std::make_shared<DefaultBaton>(opCtx.get());
    }());

    // We must prevent changing the storage engine while setting a new opCtx on the client.
    auto lk = _storageChangeMutex.readLock();

    onCreate(opCtx.get(), _clientObservers);
    ScopeGuard onCreateAndBatonGuard([&] {
        onDestroy(opCtx.get(), _clientObservers);
        opCtx->getBaton()->detach();
    });

    {
        ClientLock lk(client);

        if (!opCtx->recoveryUnit_DO_NOT_USE()) {
            opCtx->setRecoveryUnit_DO_NOT_USE(std::make_unique<RecoveryUnitNoop>(),
                                              WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
                                              lk);
        }

        // If we have a previous operation context, it's not worth crashing the process in
        // production. However, we do want to prevent it from doing more work and complain
        // loudly.
        auto lastOpCtx = client->getOperationContext();
        if (lastOpCtx) {
            killOperation(lk, lastOpCtx, ErrorCodes::Error(4946800));
            tasserted(4946801,
                      "Client has attempted to create a new OperationContext, but it already "
                      "has one");
        }

        client->_setOperationContext(opCtx.get());
    }

    onCreateAndBatonGuard.dismiss();

    return UniqueOperationContext(opCtx.release());
};

void ServiceContext::OperationContextDeleter::operator()(OperationContext* opCtx) const {
    auto client = opCtx->getClient();
    invariant(client);

    auto service = client->getServiceContext();
    invariant(service);

    onDestroy(opCtx, service->_clientObservers);
    service->_delistOperation(opCtx);
    opCtx->getBaton()->detach();

    delete opCtx;
}

ClientLock ServiceContext::getLockedClient(OperationId id) {
    return OperationIdManager::get(this).findAndLockClient(id);
}

void ServiceContext::registerClientObserver(std::unique_ptr<ClientObserver> observer) {
    _clientObservers.emplace_back(std::move(observer));
}

/**
 * TODO SERVER-85991 Once the _clients field in ServiceContext is moved to Service, change the
 * implementation here to just iterate over the _clients list directly.
 */
ClientLock Service::LockedClientsCursor::next() {
    while (auto client = _cursor.next()) {
        ClientLock lk(client);
        if (lk->getService() == _service) {
            return lk;
        }
    }
    return {};
}

void ServiceContext::setKillAllOperations(
    std::function<bool(const StringData)> excludedClientPredicate) {
    ServiceContextLock svcCtxLock(this);

    // Ensure that all newly created operation contexts will immediately be in the interrupted state
    _globalKill.store(true);
    auto opsKilled = 0;

    // Interrupt all active operations
    for (auto& [client, _] : _clients) {
        ClientLock lk(client);

        // Do not kill operations from the excluded clients.
        if (excludedClientPredicate && excludedClientPredicate(client->desc())) {
            continue;
        }

        auto opCtxToKill = client->getOperationContext();
        if (opCtxToKill) {
            killOperation(lk, opCtxToKill, ErrorCodes::InterruptedAtShutdown);
            opsKilled++;
        }
    }

    // Shared by mongos and mongod shutdown code paths
    LOGV2(4695300, "Interrupted all currently running operations", "opsKilled"_attr = opsKilled);

    // Notify any listeners who need to react to the server shutting down
    for (const auto listener : _killOpListeners) {
        try {
            listener->interruptAll(svcCtxLock);
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::killOperation(ClientLock& clientLock,
                                   OperationContext* opCtx,
                                   ErrorCodes::Error killCode) {
    opCtx->markKilled(killCode);

    for (const auto listener : _killOpListeners) {
        try {
            listener->interrupt(clientLock, opCtx);
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::_delistOperation(OperationContext* opCtx) {
    auto client = opCtx->getClient();
    {
        stdx::lock_guard clientLock(*client);
        if (!client->getOperationContext()) {
            // We've already delisted this operation.
            return;
        }
        // Assigning a new opCtx to the client must never precede the destruction of any existing
        // opCtx that references the client.
        invariant(client->getOperationContext() == opCtx);
        client->_setOperationContext({});
    }
    opCtx->releaseOperationKey();
}

void ServiceContext::delistOperation(OperationContext* opCtx) {
    auto client = opCtx->getClient();
    invariant(client);

    auto service = client->getServiceContext();
    invariant(service == this);

    _delistOperation(opCtx);
}

void ServiceContext::killAndDelistOperation(OperationContext* opCtx, ErrorCodes::Error killCode) {

    auto client = opCtx->getClient();
    invariant(client);

    auto service = client->getServiceContext();
    invariant(service == this);

    _delistOperation(opCtx);

    ClientLock clientLock(client);
    killOperation(clientLock, opCtx, killCode);
}

void ServiceContext::unsetKillAllOperations() {
    _globalKill.store(false);
}

void ServiceContext::registerKillOpListener(KillOpListenerInterface* listener) {
    stdx::lock_guard clientLock(_mutex);
    _killOpListeners.push_back(listener);
}

void ServiceContext::waitForStartupComplete() {
    stdx::unique_lock lk(_mutex);
    _startupCompleteCondVar.wait(lk, [this] { return _startupComplete; });
}

void ServiceContext::notifyStorageStartupRecoveryComplete() {
    stdx::unique_lock lk(_mutex);
    _startupComplete = true;
    lk.unlock();
    _startupCompleteCondVar.notify_all();
}

template <typename T>
ConstructorActionRegistererType<T>::ConstructorActionRegistererType(std::string name,
                                                                    ConstructorAction constructor,
                                                                    DestructorAction destructor)
    : ConstructorActionRegistererType(
          std::move(name), {}, std::move(constructor), std::move(destructor)) {}

template <typename T>
ConstructorActionRegistererType<T>::ConstructorActionRegistererType(
    std::string name,
    std::vector<std::string> prereqs,
    ConstructorAction constructor,
    DestructorAction destructor)
    : ConstructorActionRegistererType(
          std::move(name), prereqs, {}, std::move(constructor), std::move(destructor)) {}

template <typename T>
ConstructorActionRegistererType<T>::ConstructorActionRegistererType(
    std::string name,
    std::vector<std::string> prereqs,
    std::vector<std::string> dependents,
    ConstructorAction constructor,
    DestructorAction destructor) {
    if (!destructor)
        destructor = [](T*) {
        };
    _registerer.emplace(
        std::move(name),
        [this, constructor, destructor](InitializerContext*) {
            _iter = ConstructorActionRegistererType::registeredConstructorActions().emplace(
                ConstructorActionRegistererType::registeredConstructorActions().end(),
                std::move(constructor),
                std::move(destructor));
        },
        [this](DeinitializerContext*) {
            ConstructorActionRegistererType::registeredConstructorActions().erase(_iter);
        },
        std::move(prereqs),
        std::move(dependents));
}

template class ConstructorActionRegistererType<ServiceContext>;
template class ConstructorActionRegistererType<Service>;

ServiceContext::UniqueServiceContext ServiceContext::make(
    std::unique_ptr<ClockSource> fastClockSource,
    std::unique_ptr<ClockSource> preciseClockSource,
    std::unique_ptr<TickSource> tickSource) {
    auto service = std::make_unique<ServiceContext>(
        fastClockSource ? std::move(fastClockSource) : std::make_unique<SystemClockSource>(),
        preciseClockSource ? std::move(preciseClockSource) : std::make_unique<SystemClockSource>(),
        tickSource ? std::move(tickSource) : makeSystemTickSource());
    onCreate(service.get(), ConstructorActionRegisterer::registeredConstructorActions());
    return UniqueServiceContext{service.release()};
}

Service::UniqueService Service::make(ServiceContext* sc, ClusterRole role) {
    auto service = std::unique_ptr<Service>(new Service(sc, role));
    onCreate(service.get(), ConstructorActionRegisterer::registeredConstructorActions());
    return UniqueService{service.release()};
}

void ServiceContext::ServiceContextDeleter::operator()(ServiceContext* sc) const {
    // TODO SERVER-86083: Make the Service DestructorActions run before the ServiceContext
    // DestructorActions, and then destruct Services after all DestructorActions have run.
    onDestroy(sc, ConstructorActionRegisterer::registeredConstructorActions());
    // Delete the Services and fire their destructor actions.
    sc->_serviceSet.reset();
    delete sc;
}

void Service::ServiceDeleter::operator()(Service* service) const {
    onDestroy(service, ConstructorActionRegisterer::registeredConstructorActions());
    delete service;
}

}  // namespace mongo
