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


#include "mongo/db/repl/primary_only_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock/vector_clock_metadata_hook.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <functional>
#include <mutex>
#include <tuple>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceSkipRebuildingInstances);
MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceHangBeforeRebuildingInstances);
MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceFailRebuildingInstances);
MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic);

namespace {
const auto _registryDecoration = ServiceContext::declareDecoration<PrimaryOnlyServiceRegistry>();

const auto _registryRegisterer =
    ReplicaSetAwareServiceRegistry::Registerer<PrimaryOnlyServiceRegistry>(
        "PrimaryOnlyServiceRegistry");

const Status kExecutorShutdownStatus(ErrorCodes::CallbackCanceled,
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
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

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
    invariant(
        ns.isConfigDB(),
        "PrimaryOnlyServices can only register a state documents namespace in the 'config' db");
    auto name = service->getServiceName();
    auto servicePtr = service.get();

    auto [_, inserted] = _servicesByName.emplace(name, std::move(service));
    invariant(inserted,
              str::stream() << "Attempted to register PrimaryOnlyService (" << name
                            << ") that is already registered");

    auto [existingServiceIt, inserted2] = _servicesByNamespace.emplace(ns, servicePtr);
    auto existingService = existingServiceIt->second;
    invariant(inserted2,
              str::stream() << "Attempted to register PrimaryOnlyService (" << name
                            << ") with state document namespace \"" << ns.toStringForErrorMsg()
                            << "\" that is already in use by service "
                            << existingService->getServiceName());
    LOGV2_INFO(
        5123008, "Successfully registered PrimaryOnlyService", "service"_attr = name, logAttrs(ns));
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
    auto it = _servicesByNamespace.find(ns);
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

    if (!replCoord || !replCoord->getSettings().isReplSet()) {
        // Unit tests may not have replication coordinator set up.
        return;
    }

    const auto stepUpOpTime = replCoord->getMyLastAppliedOpTime();
    invariant(term == stepUpOpTime.getTerm(),
              str::stream() << "Term from last optime (" << stepUpOpTime.getTerm()
                            << ") doesn't match the term we're stepping up in (" << term << ")");


    // Since this method is called before we mark the node writable in
    // ReplicationCoordinatorImpl::signalApplierDrainComplete and therefore can block the new
    // primary from starting to receive writes, generate a warning if we are spending too long here.
    Timer totalTime{};
    ON_BLOCK_EXIT([&] {
        auto timeSpent = totalTime.millis();
        auto threshold = slowTotalOnStepUpCompleteThresholdMS.load();
        if (timeSpent > threshold) {
            LOGV2(6699604,
                  "Duration spent in PrimaryOnlyServiceRegistry::onStepUpComplete for all services "
                  "exceeded slowTotalOnStepUpCompleteThresholdMS",
                  "thresholdMillis"_attr = threshold,
                  "durationMillis"_attr = timeSpent);
        }
    });
    for (auto& service : _servicesByName) {
        // Additionally, generate a warning if any individual service is taking too long.
        Timer t{};
        ON_BLOCK_EXIT([&] {
            auto timeSpent = t.millis();
            auto threshold = slowServiceOnStepUpCompleteThresholdMS.load();
            if (timeSpent > threshold) {
                LOGV2(6699605,
                      "Duration spent in PrimaryOnlyServiceRegistry::onStepUpComplete "
                      "for service exceeded slowServiceOnStepUpCompleteThresholdMS",
                      "thresholdMillis"_attr = threshold,
                      "durationMillis"_attr = timeSpent,
                      "serviceName"_attr = service.first);
            }
        });
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
        BSONObjBuilder serviceInfoBuilder(subBuilder.subobjStart(service.first));
        service.second->reportForServerStatus(&serviceInfoBuilder);
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

void PrimaryOnlyService::reportForServerStatus(BSONObjBuilder* result) noexcept {
    stdx::lock_guard lk(_mutex);
    result->append("state", _getStateString(lk));
    result->appendNumber("numInstances", static_cast<long long>(_activeInstances.size()));
}

void PrimaryOnlyService::reportInstanceInfoForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode,
    std::vector<BSONObj>* ops) noexcept {

    stdx::lock_guard lk(_mutex);
    for (auto& [_, instance] : _activeInstances) {
        auto op = instance.getInstance()->reportForCurrentOp(connMode, sessionMode);
        if (op.has_value()) {
            ops->push_back(std::move(op.value()));
        }
    }
}

void PrimaryOnlyService::registerOpCtx(OperationContext* opCtx, bool allowOpCtxWhileRebuilding) {
    stdx::lock_guard lk(_mutex);
    auto [_, inserted] = _opCtxs.emplace(opCtx);
    invariant(inserted);

    if (_state == State::kRunning || (_state == State::kRebuilding && allowOpCtxWhileRebuilding)) {
        // We do not allow creating an opCtx while in kRebuilding (unless the thread has explicitly
        // requested it) in case the node has stepped down and back up. In that case the second
        // stepup would join the instance from the first stepup which could wait on the opCtx which
        // would not get interrupted. This could cause the second stepup to take a long time
        // to join the old instance. Note that opCtx's created through a
        // CancelableOperationContextFactory with a cancellation token *would* be interrupted (and
        // would not delay the join), because the stepdown would have cancelled the cancellation
        // token.
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
        Client::initThread(threadName,
                           getGlobalServiceContext()->getService(ClusterRole::ShardServer));
        auto client = Client::getCurrent();
        AuthorizationSession::get(*client)->grantInternalAuthorization();

        // Associate this Client with this PrimaryOnlyService
        primaryOnlyServiceStateForClient(client).primaryOnlyService = this;
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(opCtx->getServiceContext()));

    stdx::lock_guard lk(_mutex);
    if (_state == State::kShutdown) {
        return;
    }

    _executor = executor::ThreadPoolTaskExecutor::create(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(getServiceName() + "Network", nullptr, std::move(hookList)));
    _setHasExecutor(lk);

    _executor->startup();
}

void PrimaryOnlyService::onStepUp(const OpTime& stepUpOpTime) {
    SimpleBSONObjUnorderedMap<ActiveInstance> savedInstances;
    invariant(_getHasExecutor());
    auto newThenOldScopedExecutor =
        std::make_shared<executor::ScopedTaskExecutor>(_executor, kExecutorShutdownStatus);

    stdx::unique_lock lk(_mutex);

    if (_state == State::kShutdown) {
        return;
    }

    auto newTerm = stepUpOpTime.getTerm();
    invariant(newTerm > _term,
              str::stream() << "term " << newTerm << " is not greater than " << _term);
    _term = newTerm;
    _setState(State::kRebuilding, lk);
    _source = CancellationSource();

    // Install a new executor, while moving the old one into 'newThenOldScopedExecutor' so it
    // can be accessed outside of _mutex.
    using std::swap;
    swap(newThenOldScopedExecutor, _scopedExecutor);
    // Don't destroy the Instances until all outstanding tasks run against them are complete.
    swap(savedInstances, _activeInstances);

    // Save off the new executor temporarily.
    auto newScopedExecutor = _scopedExecutor;

    lk.unlock();

    // Ensure that all tasks from the previous term have completed before allowing tasks to be
    // scheduled on the new executor.
    if (newThenOldScopedExecutor) {
        // shutdown() happens in onStepDown of previous term, so we only need to join() here.
        (*newThenOldScopedExecutor)->join();
    }

    // This ensures that all instances from previous term have joined.
    for (auto& instance : savedInstances) {
        instance.second.waitForCompletion();
    }

    savedInstances.clear();
    newThenOldScopedExecutor.reset();

    _onServiceInitialization();

    PrimaryOnlyServiceHangBeforeLaunchingStepUpLogic.pauseWhileSet();

    // Now wait for the first write of the new term to be majority committed, so that we know
    // all previous writes to state documents are also committed, and then schedule work to
    // rebuild Instances from their persisted state documents.
    lk.lock();
    LOGV2_DEBUG(5601000,
                2,
                "Waiting on first write of the new term to be majority committed",
                "service"_attr = getServiceName(),
                "stepUpOpTime"_attr = stepUpOpTime);
    WaitForMajorityService::get(_serviceContext)
        .waitUntilMajorityForWrite(stepUpOpTime, _source.token())
        .thenRunOn(**newScopedExecutor)
        .then([this, newScopedExecutor, newTerm] {
            // Note that checking both the state and the term are optimizations and are
            // not strictly necessary. This is also true in the later continuation.
            {
                stdx::lock_guard lk(_mutex);
                if (_state != State::kRebuilding || _term != newTerm) {
                    return ExecutorFuture<void>(**newScopedExecutor, Status::OK());
                }
            }

            if (MONGO_unlikely(PrimaryOnlyServiceSkipRebuildingInstances.shouldFail())) {
                return ExecutorFuture<void>(**newScopedExecutor, Status::OK());
            }

            return _rebuildService(newScopedExecutor, _source.token());
        })
        .then([this, newScopedExecutor, newTerm] {
            {
                stdx::lock_guard lk(_mutex);
                if (_state != State::kRebuilding || _term != newTerm) {
                    return;
                }
            }
            _rebuildInstances(newTerm);
        })
        .onError([this, newTerm](Status s) {
            LOGV2_ERROR(5165001,
                        "Failed to rebuild PrimaryOnlyService on stepup.",
                        "service"_attr = getServiceName(),
                        "error"_attr = s);

            stdx::lock_guard lk(_mutex);
            if (_state != State::kRebuilding || _term != newTerm) {
                // We've either stepped or shut down, or advanced to a new term.
                // In either case, we rely on the stepdown/shutdown logic or the
                // step-up of the new term to set _state and do nothing here.
                bool steppedDown = _state == State::kPaused;
                bool shutDown = _state == State::kShutdown;
                bool termAdvanced = _term > newTerm;
                invariant(steppedDown || shutDown || termAdvanced,
                          fmt::format(
                              "Unexpected _state or _term; _state is {}, _term is {}, term was {} ",
                              _getStateString(lk),
                              _term,
                              newTerm));
                return;
            }
            invariant(_state == State::kRebuilding);
            _rebuildStatus = s;
            _setState(State::kRebuildFailed, lk);
        })
        .getAsync([](auto&&) {});  // Ignore the result Future
    lk.unlock();
}

void PrimaryOnlyService::_interruptInstances(WithLock, Status status) {
    _source.cancel();

    if (_scopedExecutor) {
        (*_scopedExecutor)->shutdown();
    }

    for (auto& [instanceId, instance] : _activeInstances) {
        instance.interrupt(status);
    }

    for (auto opCtx : _opCtxs) {
        ClientLock clientLock(opCtx->getClient());
        _serviceContext->killOperation(clientLock, opCtx, status.code());
    }
}

void PrimaryOnlyService::onStepDown() {
    stdx::lock_guard lk(_mutex);
    if (_state == State::kShutdown) {
        return;
    }

    LOGV2_INFO(5123007,
               "Interrupting PrimaryOnlyService due to stepDown",
               "service"_attr = getServiceName(),
               "numInstances"_attr = _activeInstances.size(),
               "numOperationContexts"_attr = _opCtxs.size());

    _onServiceTermination();
    _interruptInstances(lk,
                        {ErrorCodes::InterruptedDueToReplStateChange,
                         "PrimaryOnlyService interrupted due to stepdown"});

    _setState(State::kPaused, lk);
    _rebuildStatus = Status::OK();
}

void PrimaryOnlyService::shutdown() {
    SimpleBSONObjUnorderedMap<ActiveInstance> savedInstances;
    std::shared_ptr<executor::TaskExecutor> savedExecutor;
    std::shared_ptr<executor::ScopedTaskExecutor> savedScopedExecutor;

    bool hasExecutor;
    {
        stdx::lock_guard lk(_mutex);
        LOGV2_INFO(5123006,
                   "Shutting down PrimaryOnlyService",
                   "service"_attr = getServiceName(),
                   "numInstances"_attr = _activeInstances.size(),
                   "numOperationContexts"_attr = _opCtxs.size());

        // If the _state is already kPaused, the instances have already been interrupted.
        if (_state != State::kPaused) {
            _onServiceTermination();
            _interruptInstances(lk,
                                {ErrorCodes::InterruptedAtShutdown,
                                 "PrimaryOnlyService interrupted due to shutdown"});
        }

        // Save the executor to join() with it outside of _mutex.
        std::swap(savedScopedExecutor, _scopedExecutor);

        // Maintain the lifetime of the instances until all outstanding tasks using them are
        // complete.
        std::swap(savedInstances, _activeInstances);

        _setState(State::kShutdown, lk);
        // shutdown can race with startup, so access _hasExecutor in this critical section.
        hasExecutor = _getHasExecutor();
    }

    if (savedScopedExecutor) {
        // Ensures all work on scoped task executor is completed; in-turn ensures that
        // Instance::_finishedNotifyFuture gets set if instance is running.
        (*savedScopedExecutor)->join();
    }

    // Ensures that the instance cleanup (if any) gets completed before shutting down the
    // parent task executor.
    for (auto& [instanceId, instance] : savedInstances) {
        instance.waitForCompletion();
    }

    if (hasExecutor) {
        _executor->shutdown();
        _executor->join();
    }
    savedInstances.clear();
}

std::pair<std::shared_ptr<PrimaryOnlyService::Instance>, bool>
PrimaryOnlyService::getOrCreateInstance(OperationContext* opCtx,
                                        BSONObj initialState,
                                        bool checkOptions) {
    const auto idElem = initialState["_id"];
    uassert(4908702,
            str::stream() << "Missing _id element when adding new instance of PrimaryOnlyService \""
                          << getServiceName() << "\"",
            !idElem.eoo());
    InstanceID instanceID = idElem.wrap().getOwned();

    stdx::unique_lock lk(_mutex);
    _waitForStateNotRebuilding(opCtx, lk);
    if (_state == State::kRebuildFailed) {
        uassertStatusOK(_rebuildStatus);
    }
    uassert(
        ErrorCodes::NotWritablePrimary,
        str::stream() << "Not Primary when trying to create a new instance of PrimaryOnlyService "
                      << getServiceName(),
        _state == State::kRunning);

    auto it = _activeInstances.find(instanceID);
    if (it != _activeInstances.end()) {
        auto foundInstance = it->second.getInstance();
        if (checkOptions) {
            foundInstance->checkIfOptionsConflict(initialState);
        }

        return {foundInstance, false};
    }

    std::vector<const Instance*> existingInstances;
    for (auto& [instanceId, instance] : _activeInstances) {
        existingInstances.emplace_back(instance.getInstance().get());
    }

    checkIfConflictsWithOtherInstances(opCtx, initialState, existingInstances);

    auto newInstance =
        _insertNewInstance(lk, constructInstance(std::move(initialState)), std::move(instanceID));
    return {newInstance, true};
}

std::pair<boost::optional<std::shared_ptr<PrimaryOnlyService::Instance>>, bool>
PrimaryOnlyService::lookupInstance(OperationContext* opCtx, const InstanceID& id) {
    // If this operation is holding any database locks, then it must have opted into getting
    // interrupted at stepdown to prevent deadlocks.
    invariant(
        !shard_role_details::getLocker(opCtx)->isLocked() ||
            opCtx->shouldAlwaysInterruptAtStepDownOrUp() ||
            shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites(),
        str::stream() << "isLocked: " << shard_role_details::getLocker(opCtx)->isLocked()
                      << ", interruptibleByStepDownOrUp: "
                      << opCtx->shouldAlwaysInterruptAtStepDownOrUp()
                      << ", globalLockConflictingWithWrites: "
                      << shard_role_details::getLocker(opCtx)
                             ->wasGlobalLockTakenInModeConflictingWithWrites());

    stdx::unique_lock lk(_mutex);
    _waitForStateNotRebuilding(opCtx, lk);

    if (_state == State::kShutdown || _state == State::kPaused) {
        return {boost::none, true};
    }
    if (_state == State::kRebuildFailed) {
        uassertStatusOK(_rebuildStatus);
    }
    invariant(_state == State::kRunning);


    auto it = _activeInstances.find(id);
    if (it == _activeInstances.end()) {
        return {boost::none, false};
    }

    return {it->second.getInstance(), false};
}

std::vector<std::shared_ptr<PrimaryOnlyService::Instance>> PrimaryOnlyService::getAllInstances(
    OperationContext* opCtx) {
    // If this operation is holding any database locks, then it must have opted into getting
    // interrupted at stepdown to prevent deadlocks.
    invariant(
        !shard_role_details::getLocker(opCtx)->isLocked() ||
            opCtx->shouldAlwaysInterruptAtStepDownOrUp() ||
            shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites(),
        str::stream() << "isLocked: " << shard_role_details::getLocker(opCtx)->isLocked()
                      << ", interruptibleByStepDownOrUp: "
                      << opCtx->shouldAlwaysInterruptAtStepDownOrUp()
                      << ", globalLockConflictingWithWrites: "
                      << shard_role_details::getLocker(opCtx)
                             ->wasGlobalLockTakenInModeConflictingWithWrites());

    std::vector<std::shared_ptr<PrimaryOnlyService::Instance>> instances;

    stdx::unique_lock lk(_mutex);
    _waitForStateNotRebuilding(opCtx, lk);

    if (_state == State::kShutdown || _state == State::kPaused) {
        return instances;
    }
    if (_state == State::kRebuildFailed) {
        uassertStatusOK(_rebuildStatus);
        return instances;
    }
    invariant(_state == State::kRunning);

    for (auto& [instanceId, instance] : _activeInstances) {
        instances.emplace_back(instance.getInstance());
    }

    return instances;
}

std::shared_ptr<executor::ScopedTaskExecutor> PrimaryOnlyService::getInstanceExecutor() const {
    return _scopedExecutor;
}

void PrimaryOnlyService::releaseInstance(const InstanceID& id, Status status) {
    auto savedInstanceNodeHandle = [&]() {
        stdx::lock_guard lk(_mutex);
        return _activeInstances.extract(id);
    }();

    if (!status.isOK() && savedInstanceNodeHandle) {
        savedInstanceNodeHandle.mapped().interrupt(std::move(status));
    }
}

void PrimaryOnlyService::releaseAllInstances(Status status) {
    auto savedInstances = [&] {
        stdx::lock_guard lk(_mutex);
        SimpleBSONObjUnorderedMap<ActiveInstance> savedInstances;
        // After this _activeInstances will be empty and savedInstances will contain
        // the contents of _activeInstances.
        std::swap(_activeInstances, savedInstances);
        return savedInstances;
    }();

    if (!status.isOK()) {
        for (auto& [instanceId, instance] : savedInstances) {
            instance.interrupt(status);
        }
    }
}

void PrimaryOnlyService::_setHasExecutor(WithLock) {
    auto hadExecutor = _hasExecutor.swap(true);
    invariant(!hadExecutor);
}

bool PrimaryOnlyService::_getHasExecutor() const {
    return _hasExecutor.load();
}

void PrimaryOnlyService::_rebuildInstances(long long term) {
    std::vector<BSONObj> stateDocuments;

    auto serviceName = getServiceName();
    LOGV2_INFO(
        5123005, "Rebuilding PrimaryOnlyService due to stepUp", "service"_attr = serviceName);

    if (!MONGO_unlikely(PrimaryOnlyServiceSkipRebuildingInstances.shouldFail())) {
        auto ns = getStateDocumentsNS();
        LOGV2_DEBUG(5123004,
                    2,
                    "Querying to look for state documents while rebuilding PrimaryOnlyService",
                    logAttrs(ns),
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

            FindCommandRequest findRequest{ns};
            auto cursor = client.find(std::move(findRequest));
            while (cursor->more()) {
                stateDocuments.push_back(cursor->nextSafe().getOwned());
            }
        } catch (DBException& e) {
            e.addContext(str::stream()
                         << "Error querying the state document collection "
                         << ns.toStringForErrorMsg() << " for service " << serviceName);
            throw;
        }
    }

    LOGV2_DEBUG(5123003,
                2,
                "Found state documents while rebuilding PrimaryOnlyService that correspond to "
                "instances of that service",
                "service"_attr = serviceName,
                "numDocuments"_attr = stateDocuments.size());

    while (MONGO_unlikely(PrimaryOnlyServiceHangBeforeRebuildingInstances.shouldFail())) {
        {
            stdx::lock_guard lk(_mutex);
            if (_state != State::kRebuilding || _term != term) {  // Node stepped down
                return;
            }
        }
        sleepmillis(100);
    }

    stdx::lock_guard lk(_mutex);
    if (_state != State::kRebuilding || _term != term) {
        // Node stepped down before finishing rebuilding service from previous stepUp.
        return;
    }
    invariant(_activeInstances.empty());
    invariant(_term == term);

    // Construct new instances using the state documents and add to _activeInstances.
    LOGV2_DEBUG(5165000,
                2,
                "Starting to construct and run instances for service",
                "service"_attr = serviceName,
                "numInstances"_attr = stateDocuments.size());
    for (auto&& doc : stateDocuments) {
        auto idElem = doc["_id"];
        fassert(4923602, !idElem.eoo());
        auto instanceID = idElem.wrap().getOwned();
        auto instance = constructInstance(std::move(doc));
        [[maybe_unused]] auto newInstance =
            _insertNewInstance(lk, std::move(instance), std::move(instanceID));
    }

    _setState(State::kRunning, lk);
}

std::shared_ptr<PrimaryOnlyService::Instance> PrimaryOnlyService::_insertNewInstance(
    WithLock wl, std::shared_ptr<Instance> instance, InstanceID instanceID) {
    CancellationSource instanceSource(_source.token());
    auto runCompleteFuture =
        ExecutorFuture<void>(**_scopedExecutor)
            .then([serviceName = getServiceName(),
                   instance,
                   scopedExecutor = _scopedExecutor,
                   token = instanceSource.token(),
                   instanceID]() mutable {
                LOGV2_DEBUG(5123002,
                            3,
                            "Starting instance of PrimaryOnlyService",
                            "service"_attr = serviceName,
                            "instanceID"_attr = instanceID);

                return instance->run(std::move(scopedExecutor), token);
            })
            // TODO SERVER-61717 remove this error handler once instance are automatically released
            // at the end of run()
            .onError<ErrorCodes::ConflictingServerlessOperation>([this, instanceID](Status status) {
                LOGV2(6531507,
                      "Removing instance due to ConflictingServerlessOperation error",
                      "instanceID"_attr = instanceID);
                releaseInstance(instanceID, Status::OK());

                return status;
            })
            .semi();

    auto [it, inserted] = _activeInstances.try_emplace(
        instanceID, std::move(instance), std::move(instanceSource), std::move(runCompleteFuture));
    invariant(inserted);
    return it->second.getInstance();
}

StringData PrimaryOnlyService::_getStateString(WithLock) const {
    switch (_state) {
        case State::kRunning:
            return "running";
        case State::kPaused:
            return "paused";
        case State::kRebuilding:
            return "rebuilding";
        case State::kRebuildFailed:
            return "rebuildFailed";
        case State::kShutdown:
            return "shutdown";
        default:
            MONGO_UNREACHABLE;
    }
}

void PrimaryOnlyService::waitForStateNotRebuilding_forTest(OperationContext* opCtx) {
    stdx::unique_lock lk(_mutex);
    _waitForStateNotRebuilding(opCtx, lk);
}

void PrimaryOnlyService::_waitForStateNotRebuilding(OperationContext* opCtx,
                                                    BasicLockableAdapter m) {

    opCtx->waitForConditionOrInterrupt(
        _stateChangeCV, m, [this]() { return _state != State::kRebuilding; });
}

void PrimaryOnlyService::_setState(State newState, WithLock) {
    if (std::exchange(_state, newState) != newState) {
        _stateChangeCV.notify_all();
    }
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
