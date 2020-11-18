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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/primary_only_service.h"

#include <utility>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceSkipRebuildingInstances);
MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceHangBeforeRebuildingInstances);
MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceFailRebuildingInstances);

namespace {
const auto _registryDecoration = ServiceContext::declareDecoration<PrimaryOnlyServiceRegistry>();

const auto _registryRegisterer =
    ReplicaSetAwareServiceRegistry::Registerer<PrimaryOnlyServiceRegistry>(
        "PrimaryOnlyServiceRegistry");

const Status kExecutorShutdownStatus(ErrorCodes::InterruptedDueToReplStateChange,
                                     "PrimaryOnlyService executor shut down due to stepDown");

const auto primaryOnlyServiceStateForClient =
    Client::declareDecoration<PrimaryOnlyService::PrimaryOnlyServiceClientState>();

/**
 * A ClientObserver that adds hooks for every time an OpCtx is created on a thread that is part of a
 * PrimaryOnlyService. OpCtxs created on PrimaryOnlyService threads get registered with the
 * associated service, which can then ensure that all associated OpCtxs get interrupted any time
 * this service is interrupted due to a replica set stepDown. Additionally, we ensure that any
 * OpCtxs created while the node is *already* stepped down get created in an already-interrupted
 * state.
 */
class PrimaryOnlyServiceClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override {}
    void onDestroyClient(Client* client) override {}

    void onCreateOperationContext(OperationContext* opCtx) override {
        auto client = opCtx->getClient();
        auto clientState = primaryOnlyServiceStateForClient(client);
        if (!clientState.primaryOnlyService) {
            // This OpCtx/Client is not a part of a PrimaryOnlyService
            return;
        }

        // Ensure this OpCtx will get interrupted at stepDown by the ReplicationCoordinator
        // machinery when it tries to take the RSTL lock. This isn't strictly necessary for
        // correctness since registering the OpCtx below will ensure that the OpCtx gets interrupted
        // at stepDown anyway, but setting this lets it get interrupted a little earlier in the
        // stepDown process.
        opCtx->setAlwaysInterruptAtStepDownOrUp();

        // Register the opCtx with the PrimaryOnlyService so it will get interrupted on stepDown. We
        // need this, and cannot simply rely on the ReplicationCoordinator to interrupt this OpCtx
        // for us, as new OpCtxs can be created on PrimaryOnlyService threads even after the
        // ReplicationCoordinator has finished its stepDown procedure.
        clientState.primaryOnlyService->registerOpCtx(opCtx,
                                                      clientState.allowOpCtxWhenServiceRebuilding);
    }

    void onDestroyOperationContext(OperationContext* opCtx) override {
        auto client = opCtx->getClient();
        auto clientState = primaryOnlyServiceStateForClient(client);
        if (!clientState.primaryOnlyService) {
            // This OpCtx/Client is not a part of a PrimaryOnlyService
            return;
        }

        clientState.primaryOnlyService->unregisterOpCtx(opCtx);
    }
};

ServiceContext::ConstructorActionRegisterer primaryOnlyServiceClientObserverRegisterer{
    "PrimaryOnlyServiceClientObserver", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<PrimaryOnlyServiceClientObserver>());
    }};

}  // namespace

PrimaryOnlyServiceRegistry* PrimaryOnlyServiceRegistry::get(ServiceContext* serviceContext) {
    return &_registryDecoration(serviceContext);
}

void PrimaryOnlyServiceRegistry::registerService(std::unique_ptr<PrimaryOnlyService> service) {
    auto ns = service->getStateDocumentsNS();
    auto name = service->getServiceName();
    auto servicePtr = service.get();

    auto [_, inserted] = _servicesByName.emplace(name, std::move(service));
    invariant(inserted,
              str::stream() << "Attempted to register PrimaryOnlyService (" << name
                            << ") that is already registered");

    auto [existingServiceIt, inserted2] = _servicesByNamespace.emplace(ns.toString(), servicePtr);
    auto existingService = existingServiceIt->second;
    invariant(inserted2,
              str::stream() << "Attempted to register PrimaryOnlyService (" << name
                            << ") with state document namespace \"" << ns
                            << "\" that is already in use by service "
                            << existingService->getServiceName());
    LOGV2_INFO(
        5123008,
        "Successfully registered PrimaryOnlyService {service} with state documents stored in {ns}",
        "Successfully registered PrimaryOnlyService",
        "service"_attr = name,
        "ns"_attr = ns);
}

PrimaryOnlyService* PrimaryOnlyServiceRegistry::lookupServiceByName(StringData serviceName) {
    auto it = _servicesByName.find(serviceName);
    invariant(it != _servicesByName.end());
    auto servicePtr = it->second.get();
    invariant(servicePtr);
    return servicePtr;
}

PrimaryOnlyService* PrimaryOnlyServiceRegistry::lookupServiceByNamespace(
    const NamespaceString& ns) {
    auto it = _servicesByNamespace.find(ns.toString());
    if (it == _servicesByNamespace.end()) {
        return nullptr;
    }
    auto servicePtr = it->second;
    invariant(servicePtr);
    return servicePtr;
}

void PrimaryOnlyServiceRegistry::onStartup(OperationContext* opCtx) {
    for (auto& service : _servicesByName) {
        service.second->startup(opCtx);
    }
}

void PrimaryOnlyServiceRegistry::onShutdown() {
    for (auto& service : _servicesByName) {
        service.second->shutdown();
    }
}

void PrimaryOnlyServiceRegistry::onStepUpComplete(OperationContext* opCtx, long long term) {
    auto replCoord = ReplicationCoordinator::get(opCtx);

    if (!replCoord || !replCoord->isReplEnabled()) {
        // Unit tests may not have replication coordinator set up.
        return;
    }

    const auto stepUpOpTime = replCoord->getMyLastAppliedOpTime();
    invariant(term == stepUpOpTime.getTerm(),
              str::stream() << "Term from last optime (" << stepUpOpTime.getTerm()
                            << ") doesn't match the term we're stepping up in (" << term << ")");

    for (auto& service : _servicesByName) {
        service.second->onStepUp(stepUpOpTime);
    }
}

void PrimaryOnlyServiceRegistry::onStepDown() {
    for (auto& service : _servicesByName) {
        service.second->onStepDown();
    }
}

void PrimaryOnlyServiceRegistry::reportServiceInfoForServerStatus(BSONObjBuilder* result) noexcept {
    BSONObjBuilder subBuilder(result->subobjStart("primaryOnlyServices"));
    for (auto& service : _servicesByName) {
        subBuilder.appendNumber(service.first, service.second->getNumberOfInstances());
    }
}

void PrimaryOnlyServiceRegistry::reportServiceInfoForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode,
    std::vector<BSONObj>* ops) noexcept {
    for (auto& [_, service] : _servicesByName) {
        service->reportInstanceInfoForCurrentOp(connMode, sessionMode, ops);
    }
}

PrimaryOnlyService::PrimaryOnlyService(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

size_t PrimaryOnlyService::getNumberOfInstances() {
    stdx::lock_guard lk(_mutex);
    return _instances.size();
}

void PrimaryOnlyService::reportInstanceInfoForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode,
    std::vector<BSONObj>* ops) noexcept {

    stdx::lock_guard lk(_mutex);
    for (auto& [_, instance] : _instances) {
        auto op = instance->reportForCurrentOp(connMode, sessionMode);
        if (op.has_value()) {
            ops->push_back(std::move(op.get()));
        }
    }
}

bool PrimaryOnlyService::isRunning() const {
    stdx::lock_guard lk(_mutex);
    return _state == State::kRunning;
}

void PrimaryOnlyService::registerOpCtx(OperationContext* opCtx, bool allowOpCtxWhileRebuilding) {
    stdx::lock_guard lk(_mutex);
    auto [_, inserted] = _opCtxs.emplace(opCtx);
    invariant(inserted);

    if (_state == State::kRunning || (_state == State::kRebuilding && allowOpCtxWhileRebuilding)) {
        return;
    } else {
        // If this service isn't running when an OpCtx associated with this service is created, then
        // ensure that the OpCtx starts off immediately interrupted.
        // We don't use ServiceContext::killOperation to avoid taking the Client lock unnecessarily,
        // given that we're running on behalf of the thread that owns the OpCtx.
        opCtx->markKilled(ErrorCodes::NotWritablePrimary);
    }
}

void PrimaryOnlyService::unregisterOpCtx(OperationContext* opCtx) {
    stdx::lock_guard lk(_mutex);
    auto wasRegistered = _opCtxs.erase(opCtx);
    invariant(wasRegistered);
}

std::shared_ptr<executor::TaskExecutor> PrimaryOnlyService::getInstanceCleanupExecutor() const {
    invariant(_getHasExecutor());
    return _executor;
}

void PrimaryOnlyService::startup(OperationContext* opCtx) {
    // Initialize the thread pool options with the service-specific limits on pool size.
    ThreadPool::Options threadPoolOptions(getThreadPoolLimits());

    // Now add the options that are fixed for all PrimaryOnlyServices.
    threadPoolOptions.threadNamePrefix = getServiceName() + "-";
    threadPoolOptions.poolName = getServiceName() + "ThreadPool";
    threadPoolOptions.onCreateThread = [this](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        auto client = Client::getCurrent();
        AuthorizationSession::get(*client)->grantInternalAuthorization(&cc());

        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);

        // Associate this Client with this PrimaryOnlyService
        primaryOnlyServiceStateForClient(client).primaryOnlyService = this;
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(opCtx->getServiceContext()));

    stdx::lock_guard lk(_mutex);
    if (_state == State::kShutdown) {
        return;
    }

    _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(getServiceName() + "Network", nullptr, std::move(hookList)));
    _setHasExecutor(lk);

    _executor->startup();
}

void PrimaryOnlyService::onStepUp(const OpTime& stepUpOpTime) {
    InstanceMap savedInstances;
    invariant(_getHasExecutor());
    auto newThenOldScopedExecutor =
        std::make_shared<executor::ScopedTaskExecutor>(_executor, kExecutorShutdownStatus);
    {
        stdx::lock_guard lk(_mutex);

        if (_state == State::kShutdown) {
            return;
        }

        auto newTerm = stepUpOpTime.getTerm();
        invariant(newTerm > _term,
                  str::stream() << "term " << newTerm << " is not greater than " << _term);
        _term = newTerm;
        _state = State::kRebuilding;

        // Install a new executor, while moving the old one into 'newThenOldScopedExecutor' so it
        // can be accessed outside of _mutex.
        using std::swap;
        swap(newThenOldScopedExecutor, _scopedExecutor);
        // Don't destroy the Instances until all outstanding tasks run against them are complete.
        swap(savedInstances, _instances);
    }

    // Ensure that all tasks from the previous term have completed before allowing tasks to be
    // scheduled on the new executor.
    if (newThenOldScopedExecutor) {
        // shutdown() happens in onStepDown of previous term, so we only need to join() here.
        (*newThenOldScopedExecutor)->join();
    }

    // This ensures that all instances from previous term have joined.
    for (auto& instance : savedInstances) {
        if (instance.second->_running)
            instance.second->_finishedNotifyFuture->wait();
    }

    // Now wait for the first write of the new term to be majority committed, so that we know
    // all previous writes to state documents are also committed, and then schedule work to
    // rebuild Instances from their persisted state documents.
    stdx::lock_guard lk(_mutex);
    auto term = _term;
    WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(stepUpOpTime)
        .thenRunOn(**_scopedExecutor)
        .then([this] {
            if (MONGO_unlikely(PrimaryOnlyServiceSkipRebuildingInstances.shouldFail())) {
                return ExecutorFuture<void>(**_scopedExecutor, Status::OK());
            }

            return _rebuildService(_scopedExecutor);
        })
        .then([this, term] { _rebuildInstances(term); })
        .getAsync([](auto&&) {});  // Ignore the result Future
}

void PrimaryOnlyService::onStepDown() {
    stdx::lock_guard lk(_mutex);
    if (_state == State::kShutdown) {
        return;
    }

    LOGV2_INFO(5123007,
               "Interrupting (due to stepDown) PrimaryOnlyService {service} with {numInstances} "
               "currently running instances and {numOperationContexts} associated operations",
               "Interrupting PrimaryOnlyService due to stepDown",
               "service"_attr = getServiceName(),
               "numInstances"_attr = _instances.size(),
               "numOperationContexts"_attr = _opCtxs.size());

    // Reset the cancelation source on stepdown to prepare for the next elected primary.
    _source.cancel();
    _source = CancelationSource();

    if (_scopedExecutor) {
        (*_scopedExecutor)->shutdown();
    }

    for (auto& instance : _instances) {
        instance.second->interrupt({ErrorCodes::InterruptedDueToReplStateChange,
                                    "PrimaryOnlyService interrupted due to stepdown"});
    }

    for (auto opCtx : _opCtxs) {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        _serviceContext->killOperation(
            clientLock, opCtx, ErrorCodes::InterruptedDueToReplStateChange);
    }

    _state = State::kPaused;
    _rebuildStatus = Status::OK();
}

void PrimaryOnlyService::shutdown() {
    InstanceMap savedInstances;
    std::shared_ptr<executor::TaskExecutor> savedExecutor;
    std::shared_ptr<executor::ScopedTaskExecutor> savedScopedExecutor;

    bool hasExecutor;
    {
        stdx::lock_guard lk(_mutex);

        LOGV2_INFO(
            5123006,
            "Shutting down PrimaryOnlyService {service} with {numInstances} currently running "
            "instances and {numOperationContexts} associated operations",
            "Shutting down PrimaryOnlyService",
            "service"_attr = getServiceName(),
            "numInstances"_attr = _instances.size(),
            "numOperationContexts"_attr = _opCtxs.size());

        // Save the executor to join() with it outside of _mutex.
        using std::swap;
        swap(savedScopedExecutor, _scopedExecutor);

        // Maintain the lifetime of the instances until all outstanding tasks using them are
        // complete.
        swap(savedInstances, _instances);

        _state = State::kShutdown;
        // shutdown can race with startup, so access _hasExecutor in this critical section.
        hasExecutor = _getHasExecutor();
    }

    _source.cancel();

    for (auto& instance : savedInstances) {
        instance.second->interrupt(
            {ErrorCodes::InterruptedAtShutdown, "PrimaryOnlyService interrupted due to shutdown"});
    }

    if (savedScopedExecutor) {
        // Make sure to shut down the scoped executor before the parent executor to avoid
        // SERVER-50612.
        (*savedScopedExecutor)->shutdown();
        // Ensures all work on scoped task executor is completed; in-turn ensures that
        // Instance::_finishedNotifyFuture gets set if instance is running.
        (*savedScopedExecutor)->join();
    }

    // Ensures that the instance cleanup (if any) gets completed before shutting down the
    // parent task executor.
    for (auto& instance : savedInstances) {
        if (instance.second->_running)
            instance.second->_finishedNotifyFuture->wait();
    }

    if (hasExecutor) {
        _executor->shutdown();
        _executor->join();
    }
    savedInstances.clear();
}

std::shared_ptr<PrimaryOnlyService::Instance> PrimaryOnlyService::getOrCreateInstance(
    OperationContext* opCtx, BSONObj initialState) {
    const auto idElem = initialState["_id"];
    uassert(4908702,
            str::stream() << "Missing _id element when adding new instance of PrimaryOnlyService \""
                          << getServiceName() << "\"",
            !idElem.eoo());
    InstanceID instanceID = idElem.wrap().getOwned();

    stdx::unique_lock lk(_mutex);
    opCtx->waitForConditionOrInterrupt(
        _rebuildCV, lk, [this]() { return _state != State::kRebuilding; });
    if (_state == State::kRebuildFailed) {
        uassertStatusOK(_rebuildStatus);
    }
    uassert(
        ErrorCodes::NotWritablePrimary,
        str::stream() << "Not Primary when trying to create a new instance of PrimaryOnlyService "
                      << getServiceName(),
        _state == State::kRunning);

    auto it = _instances.find(instanceID);
    if (it != _instances.end()) {
        return it->second;
    }
    auto [it2, inserted] =
        _instances.emplace(instanceID, constructInstance(std::move(initialState)));
    invariant(inserted);

    // Kick off async work to run the instance
    _scheduleRun(lk, it2->second, instanceID);

    return it2->second;
}

boost::optional<std::shared_ptr<PrimaryOnlyService::Instance>> PrimaryOnlyService::lookupInstance(
    OperationContext* opCtx, const InstanceID& id) {
    // If this operation is holding any database locks, then it must have opted into getting
    // interrupted at stepdown to prevent deadlocks.
    invariant(!opCtx->lockState()->isLocked() || opCtx->shouldAlwaysInterruptAtStepDownOrUp() ||
              opCtx->lockState()->wasGlobalLockTakenInModeConflictingWithWrites());

    stdx::unique_lock lk(_mutex);
    opCtx->waitForConditionOrInterrupt(
        _rebuildCV, lk, [this]() { return _state != State::kRebuilding; });

    if (_state == State::kShutdown || _state == State::kPaused) {
        invariant(_instances.empty());
        return boost::none;
    }
    if (_state == State::kRebuildFailed) {
        uassertStatusOK(_rebuildStatus);
    }
    invariant(_state == State::kRunning);


    auto it = _instances.find(id);
    if (it == _instances.end()) {
        return boost::none;
    }

    return it->second;
}

void PrimaryOnlyService::releaseInstance(const InstanceID& id) {
    stdx::lock_guard lk(_mutex);
    _instances.erase(id);
}

void PrimaryOnlyService::releaseAllInstances() {
    stdx::lock_guard lk(_mutex);
    _instances.clear();
}

void PrimaryOnlyService::_setHasExecutor(WithLock) {
    auto hadExecutor = _hasExecutor.swap(true);
    invariant(!hadExecutor);
}

bool PrimaryOnlyService::_getHasExecutor() const {
    return _hasExecutor.load();
}

void PrimaryOnlyService::_rebuildInstances(long long term) noexcept {
    std::vector<BSONObj> stateDocuments;

    auto serviceName = getServiceName();
    LOGV2_INFO(5123005,
               "Rebuilding PrimaryOnlyService {service} due to stepUp",
               "Rebuilding PrimaryOnlyService due to stepUp",
               "service"_attr = serviceName);

    if (!MONGO_unlikely(PrimaryOnlyServiceSkipRebuildingInstances.shouldFail())) {
        auto ns = getStateDocumentsNS();
        LOGV2_DEBUG(5123004,
                    2,
                    "Querying {ns} to look for state documents while rebuilding PrimaryOnlyService "
                    "{service}",
                    "Querying to look for state documents while rebuiding PrimaryOnlyService",
                    "ns"_attr = ns,
                    "service"_attr = serviceName);

        // The PrimaryOnlyServiceClientObserver will make any OpCtx created as part of a
        // PrimaryOnlyService immediately get interrupted if the service is not in state kRunning.
        // Since we are in State::kRebuilding here, we need to install a
        // AllowOpCtxWhenServiceNotRunningBlock so that the database read we need to do can complete
        // successfully.
        AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        try {
            if (MONGO_unlikely(PrimaryOnlyServiceFailRebuildingInstances.shouldFail())) {
                uassertStatusOK(
                    Status(ErrorCodes::InternalError, "Querying state documents failed"));
            }

            auto cursor = client.query(ns, Query());
            while (cursor->more()) {
                stateDocuments.push_back(cursor->nextSafe().getOwned());
            }
        } catch (const DBException& e) {
            LOGV2_ERROR(
                4923601,
                "Failed to start PrimaryOnlyService {service} because the query on {ns} "
                "for state documents failed due to {error}",
                "Failed to start PrimaryOnlyService because the query for state documents failed",
                "service"_attr = serviceName,
                "ns"_attr = ns,
                "error"_attr = e);

            Status status = e.toStatus();
            status.addContext(str::stream()
                              << "Failed to start PrimaryOnlyService \"" << serviceName
                              << "\" because the query for state documents on ns \"" << ns
                              << "\" failed");

            stdx::lock_guard lk(_mutex);
            if (_state != State::kRebuilding || _term != term) {
                _rebuildCV.notify_all();
                return;
            }
            _state = State::kRebuildFailed;
            _rebuildStatus = std::move(status);
            _rebuildCV.notify_all();
            return;
        }
    }

    while (MONGO_unlikely(PrimaryOnlyServiceHangBeforeRebuildingInstances.shouldFail())) {
        {
            stdx::lock_guard lk(_mutex);
            if (_state != State::kRebuilding || _term != term) {  // Node stepped down
                _rebuildCV.notify_all();
                return;
            }
        }
        sleepmillis(100);
    }

    stdx::lock_guard lk(_mutex);
    if (_state != State::kRebuilding || _term != term) {
        // Node stepped down before finishing rebuilding service from previous stepUp.
        _rebuildCV.notify_all();
        return;
    }
    invariant(_instances.empty());
    invariant(_term == term);

    LOGV2_DEBUG(5123003,
                2,
                "While rebuilding PrimaryOnlyService {service}, found {numDocuments} state "
                "documents corresponding to instances of that service",
                "Found state documents while rebuilding PrimaryOnlyService that correspond to "
                "instances of that service",
                "service"_attr = serviceName,
                "numDocuments"_attr = stateDocuments.size());

    for (auto&& doc : stateDocuments) {
        auto idElem = doc["_id"];
        fassert(4923602, !idElem.eoo());
        auto instanceID = idElem.wrap().getOwned();
        auto instance = constructInstance(std::move(doc));

        auto [_, inserted] = _instances.emplace(instanceID, instance);
        invariant(inserted);
        _scheduleRun(lk, std::move(instance), instanceID);
    }
    _state = State::kRunning;
    _rebuildCV.notify_all();
}

void PrimaryOnlyService::_scheduleRun(WithLock wl,
                                      std::shared_ptr<Instance> instance,
                                      InstanceID instanceID) {
    (*_scopedExecutor)
        ->schedule([this,
                    instance = std::move(instance),
                    scopedExecutor = _scopedExecutor,
                    instanceID](auto status) {
            if (ErrorCodes::isCancelationError(status) ||
                ErrorCodes::InterruptedDueToReplStateChange ==  // from kExecutorShutdownStatus
                    status) {
                return;
            }
            invariant(status);

            invariant(!instance->_running);
            instance->_running = true;

            LOGV2_DEBUG(
                5123002,
                3,
                "Starting instance of PrimaryOnlyService {service} with InstanceID {instanceID}",
                "Starting instance of PrimaryOnlyService",
                "service"_attr = getServiceName(),
                "instanceID"_attr = instanceID);
            instance->_source = CancelationSource(_source.token());
            instance->_finishedNotifyFuture =
                instance->run(std::move(scopedExecutor), instance->_source.token());
        });
}

PrimaryOnlyService::AllowOpCtxWhenServiceRebuildingBlock::AllowOpCtxWhenServiceRebuildingBlock(
    Client* client)
    : _client(client), _clientState(&primaryOnlyServiceStateForClient(_client)) {
    invariant(_clientState->primaryOnlyService);
    invariant(_clientState->allowOpCtxWhenServiceRebuilding == false);
    _clientState->allowOpCtxWhenServiceRebuilding = true;
}

PrimaryOnlyService::AllowOpCtxWhenServiceRebuildingBlock::~AllowOpCtxWhenServiceRebuildingBlock() {
    invariant(_clientState->allowOpCtxWhenServiceRebuilding == true);
    _clientState->allowOpCtxWhenServiceRebuilding = false;
}

}  // namespace repl
}  // namespace mongo
