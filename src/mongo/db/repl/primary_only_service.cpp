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
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replica_set_aware_service.h"
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

MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceHangBeforeCreatingInstance);

namespace {
const auto _registryDecoration = ServiceContext::declareDecoration<PrimaryOnlyServiceRegistry>();

const auto _registryRegisterer =
    ReplicaSetAwareServiceRegistry::Registerer<PrimaryOnlyServiceRegistry>(
        "PrimaryOnlyServiceRegistry");

const Status kExecutorShutdownStatus(ErrorCodes::InterruptedDueToReplStateChange,
                                     "PrimaryOnlyService executor shut down due to stepDown");

// Throws on error.
void insertDocument(OperationContext* opCtx,
                    const NamespaceString& collectionName,
                    const BSONObj& document) {
    DBDirectClient client(opCtx);

    BSONObj res;
    client.runCommand(collectionName.db().toString(),
                      [&] {
                          write_ops::Insert insertOp(collectionName);
                          insertOp.setDocuments({document});
                          return insertOp.toBSON({});
                      }(),
                      res);

    BatchedCommandResponse response;
    std::string errmsg;
    invariant(response.parseBSON(res, &errmsg));
    uassertStatusOK(response.toStatus());
}
}  // namespace

PrimaryOnlyServiceRegistry* PrimaryOnlyServiceRegistry::get(ServiceContext* serviceContext) {
    return &_registryDecoration(serviceContext);
}

void PrimaryOnlyServiceRegistry::registerService(std::unique_ptr<PrimaryOnlyService> service) {
    auto name = service->getServiceName();
    auto [_, inserted] = _services.emplace(name, std::move(service));
    invariant(inserted,
              str::stream() << "Attempted to register PrimaryOnlyService (" << name
                            << ") that is already registered");
}

PrimaryOnlyService* PrimaryOnlyServiceRegistry::lookupService(StringData serviceName) {
    auto it = _services.find(serviceName);
    invariant(it != _services.end());
    auto servicePtr = it->second.get();
    invariant(servicePtr);
    return servicePtr;
}

void PrimaryOnlyServiceRegistry::onStartup(OperationContext* opCtx) {
    for (auto& service : _services) {
        service.second->startup(opCtx);
    }
}

void PrimaryOnlyServiceRegistry::onStepUpComplete(OperationContext*, long long term) {
    for (auto& service : _services) {
        service.second->onStepUp(term);
    }
}

void PrimaryOnlyServiceRegistry::onStepDown() {
    for (auto& service : _services) {
        service.second->onStepDown();
    }
}

void PrimaryOnlyServiceRegistry::shutdown() {
    for (auto& service : _services) {
        service.second->shutdown();
    }
}

PrimaryOnlyService::PrimaryOnlyService(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

void PrimaryOnlyService::startup(OperationContext* opCtx) {
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.threadNamePrefix = getServiceName() + "-";
    threadPoolOptions.poolName = getServiceName() + "ThreadPool";
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(opCtx->getServiceContext()));
    _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(getServiceName() + "Network", nullptr, std::move(hookList)));
    _executor->startup();
}

void PrimaryOnlyService::onStepUp(long long term) {
    InstanceMap savedInstances;
    auto newThenOldScopedExecutor =
        std::make_shared<executor::ScopedTaskExecutor>(_executor, kExecutorShutdownStatus);
    {
        stdx::lock_guard lk(_mutex);

        invariant(term > _term,
                  str::stream() << "term " << term << " is not greater than " << _term);
        _term = term;
        _state = State::kRunning;

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
        // Shutdown happens in onStepDown of previous term, so we only need to join() here.
        (*newThenOldScopedExecutor)->join();
    }
}

void PrimaryOnlyService::onStepDown() {
    stdx::lock_guard lk(_mutex);

    if (_scopedExecutor) {
        (*_scopedExecutor)->shutdown();
    }
    _state = State::kPaused;
}

void PrimaryOnlyService::shutdown() {
    InstanceMap savedInstances;
    std::shared_ptr<executor::TaskExecutor> savedExecutor;

    {
        stdx::lock_guard lk(_mutex);

        // Save the executor to join() with it outside of _mutex.
        using std::swap;
        swap(savedExecutor, _executor);
        // Maintain the lifetime of the instances until all outstanding tasks using them are
        // complete.
        swap(savedInstances, _instances);

        _scopedExecutor.reset();
        _state = State::kShutdown;
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

    stdx::lock_guard lk(_mutex);
    uassert(
        ErrorCodes::NotMaster,
        str::stream() << "Not Primary when trying to create a new instance of PrimaryOnlyService "
                      << getServiceName(),
        _state == State::kRunning);

    auto it = _instances.find(instanceID);
    if (it != _instances.end()) {
        return it->second;
    }
    auto [it2, inserted] =
        _instances.emplace(instanceID.getOwned(), constructInstance(std::move(initialState)));
    invariant(inserted);

    // Kick off async work to run the instance
    it2->second->scheduleRun(_scopedExecutor);

    return it2->second;
}

boost::optional<std::shared_ptr<PrimaryOnlyService::Instance>> PrimaryOnlyService::lookupInstance(
    const InstanceID& id) {
    stdx::lock_guard lk(_mutex);

    auto it = _instances.find(id);
    if (it == _instances.end()) {
        return boost::none;
    }

    return it->second;
}

void PrimaryOnlyService::Instance::scheduleRun(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    invariant(!_running);
    _running = true;

    (*executor)->schedule([this, executor = std::move(executor)](auto status) {
        if (ErrorCodes::isCancelationError(status) || ErrorCodes::NotMaster == status) {
            return;
        }
        invariant(status);

        run(std::move(executor));
    });
}

}  // namespace repl
}  // namespace mongo
