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

/**
 * Client decoration used by Clients that are a part of a PrimaryOnlyService.
 */
struct PrimaryOnlyServiceClientState {
    PrimaryOnlyService* primaryOnlyService = nullptr;
    bool allowOpCtxWhenServiceNotRunning = false;
};

const auto primaryOnlyServiceStateForClient =
    Client::declareDecoration<PrimaryOnlyServiceClientState>();

/**
 * A ClientObserver that adds a hook for every time an OpCtx is created on a thread that is part of
 * a PrimaryOnlyService and ensures that the OpCtx is immediately interrupted if the associated
 * service is not running at the time that the OpCtx is created.  This protects against the case
 * where work for a service is scheduled and then the node steps down and back up before the work
 * creates an OpCtx. This works because even though the node has stepped back up already, the
 * service isn't "running" until it's finished its recovery which involves waiting for all work
 * from the previous term as primary to complete.
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

        // Ensure this OpCtx will get interrupted at stepDown.
        opCtx->setAlwaysInterruptAtStepDownOrUp();

        // If the PrimaryOnlyService this OpCtx is a part of isn't running when it's created, then
        // ensure the OpCtx starts off immediately interrupted.
        if (!clientState.allowOpCtxWhenServiceNotRunning &&
            !clientState.primaryOnlyService->isRunning()) {
            opCtx->markKilled(ErrorCodes::NotWritablePrimary);
        }
    }
    void onDestroyOperationContext(OperationContext* opCtx) override {}
};

ServiceContext::ConstructorActionRegisterer primaryOnlyServiceClientObserverRegisterer{
    "PrimaryOnlyServiceClientObserver", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<PrimaryOnlyServiceClientObserver>());
    }};

/**
 * Allows OpCtxs created on PrimaryOnlyService threads to remain uninterrupted, even if the service
 * they are associated with isn't running. Used during the stepUp process to allow the database
 * read required to rebuild a service and get it running in the first place.
 * Does not prevent other forms of OpCtx interruption, such as from stepDown or calls to killOp.
 */
class AllowOpCtxWhenServiceNotRunningBlock {
public:
    explicit AllowOpCtxWhenServiceNotRunningBlock(Client* client)
        : _client(client), _clientState(&primaryOnlyServiceStateForClient(_client)) {
        invariant(_clientState->primaryOnlyService);
        invariant(_clientState->allowOpCtxWhenServiceNotRunning == false);
        _clientState->allowOpCtxWhenServiceNotRunning = true;
    }
    ~AllowOpCtxWhenServiceNotRunningBlock() {
        invariant(_clientState->allowOpCtxWhenServiceNotRunning == true);
        _clientState->allowOpCtxWhenServiceNotRunning = false;
    }

private:
    Client* _client;
    PrimaryOnlyServiceClientState* _clientState;
};

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
    _executor->startup();
}

void PrimaryOnlyService::onStepUp(const OpTime& stepUpOpTime) {
    InstanceMap savedInstances;
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

    // Now wait for the first write of the new term to be majority committed, so that we know all
    // previous writes to state documents are also committed, and then schedule work to rebuild
    // Instances from their persisted state documents.
    stdx::lock_guard lk(_mutex);
    auto term = _term;
    WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(stepUpOpTime)
        .thenRunOn(**_scopedExecutor)
        .then([this, term] { _rebuildInstances(term); })
        .getAsync([](auto&&) {});  // Ignore the result Future
}

void PrimaryOnlyService::onStepDown() {
    stdx::lock_guard lk(_mutex);
    if (_state == State::kShutdown) {
        return;
    }

    for (auto& instance : _instances) {
        instance.second->interrupt({ErrorCodes::InterruptedDueToReplStateChange,
                                    "PrimaryOnlyService interrupted due to stepdown"});
    }

    if (_scopedExecutor) {
        (*_scopedExecutor)->shutdown();
    }

    _state = State::kPaused;
    _rebuildStatus = Status::OK();
}

void PrimaryOnlyService::shutdown() {
    InstanceMap savedInstances;
    std::shared_ptr<executor::TaskExecutor> savedExecutor;
    std::shared_ptr<executor::ScopedTaskExecutor> savedScopedExecutor;

    {
        stdx::lock_guard lk(_mutex);

        // Save the executor to join() with it outside of _mutex.
        using std::swap;
        swap(savedScopedExecutor, _scopedExecutor);
        swap(savedExecutor, _executor);

        // Maintain the lifetime of the instances until all outstanding tasks using them are
        // complete.
        swap(savedInstances, _instances);

        _state = State::kShutdown;
    }

    for (auto& instance : savedInstances) {
        instance.second->interrupt(
            {ErrorCodes::InterruptedAtShutdown, "PrimaryOnlyService interrupted due to shutdown"});
    }

    if (savedScopedExecutor) {
        // Make sure to shut down the scoped executor before the parent executor to avoid
        // SERVER-50612.
        (*savedScopedExecutor)->shutdown();
        // No need to join() here since joining the parent executor below will join with all tasks
        // owned by the scoped executor.
    }

    if (savedExecutor) {
        savedExecutor->shutdown();
        savedExecutor->join();
    }
    savedInstances.clear();
}

std::shared_ptr<PrimaryOnlyService::Instance> PrimaryOnlyService::getOrCreateInstance(
    BSONObj initialState) {
    const auto idElem = initialState["_id"];
    uassert(4908702,
            str::stream() << "Missing _id element when adding new instance of PrimaryOnlyService \""
                          << getServiceName() << "\"",
            !idElem.eoo());
    InstanceID instanceID = idElem.wrap();

    stdx::unique_lock lk(_mutex);
    while (_state == State::kRebuilding) {
        _rebuildCV.wait(lk);
    }
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
    auto [it2, inserted] = _instances.emplace(std::move(instanceID).getOwned(),
                                              constructInstance(std::move(initialState)));
    invariant(inserted);

    // Kick off async work to run the instance
    _scheduleRun(lk, it2->second);

    return it2->second;
}

boost::optional<std::shared_ptr<PrimaryOnlyService::Instance>> PrimaryOnlyService::lookupInstance(
    OperationContext* opCtx, const InstanceID& id) {
    // If this operation is holding any database locks, then it must have opted into getting
    // interrupted at stepdown to prevent deadlocks.
    invariant(!opCtx->lockState()->isLocked() || opCtx->shouldAlwaysInterruptAtStepDownOrUp());

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

void PrimaryOnlyService::_rebuildInstances(long long term) noexcept {
    if (MONGO_unlikely(PrimaryOnlyServiceSkipRebuildingInstances.shouldFail())) {
        return;
    }

    std::vector<BSONObj> stateDocuments;
    {
        // The PrimaryOnlyServiceClientObserver will make any OpCtx created as part of a
        // PrimaryOnlyService immediately get interrupted if the service is not in state kRunning.
        // Since we are in State::kRebuilding here, we need to install a
        // AllowOpCtxWhenServiceNotRunningBlock so that the database read we need to do can complete
        // successfully.
        AllowOpCtxWhenServiceNotRunningBlock allowOpCtxBlock(Client::getCurrent());
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        try {
            if (MONGO_unlikely(PrimaryOnlyServiceFailRebuildingInstances.shouldFail())) {
                uassertStatusOK(
                    Status(ErrorCodes::InternalError, "Querying state documents failed"));
            }

            auto cursor = client.query(getStateDocumentsNS(), Query());
            while (cursor->more()) {
                stateDocuments.push_back(cursor->nextSafe().getOwned());
            }
        } catch (const DBException& e) {
            LOGV2_ERROR(4923601,
                        "Failed to start PrimaryOnlyService {service} because the query on {ns} "
                        "for state documents failed due to {error}",
                        "ns"_attr = getStateDocumentsNS(),
                        "service"_attr = getServiceName(),
                        "error"_attr = e);

            Status status = e.toStatus();
            status.addContext(str::stream()
                              << "Failed to start PrimaryOnlyService \"" << getServiceName()
                              << "\" because the query for state documents on ns \""
                              << getStateDocumentsNS() << "\" failed");

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

    for (auto&& doc : stateDocuments) {
        auto idElem = doc["_id"];
        fassert(4923602, !idElem.eoo());
        auto instanceID = idElem.wrap().getOwned();
        auto instance = constructInstance(std::move(doc));

        auto [_, inserted] = _instances.emplace(instanceID, instance);
        invariant(inserted);
        _scheduleRun(lk, std::move(instance));
    }
    _state = State::kRunning;
    _rebuildCV.notify_all();
}

void PrimaryOnlyService::_scheduleRun(WithLock wl, std::shared_ptr<Instance> instance) {
    (*_scopedExecutor)
        ->schedule([this,
                    instance = std::move(instance),
                    scopedExecutor = _scopedExecutor,
                    executor = _executor](auto status) {
            if (ErrorCodes::isCancelationError(status) ||
                ErrorCodes::InterruptedDueToReplStateChange ==  // from kExecutorShutdownStatus
                    status) {
                return;
            }
            invariant(status);

            invariant(!instance->_running);
            instance->_running = true;

            instance->run(std::move(scopedExecutor));
        });
}

}  // namespace repl
}  // namespace mongo
