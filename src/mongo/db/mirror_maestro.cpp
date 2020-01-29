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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/mirror_maestro.h"

#include <utility>

#include <fmt/format.h>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

class MirrorMaestroImpl {
public:
    /**
     * Make the TaskExecutor and initialize other components
     */
    void init(ServiceContext* serviceContext) noexcept;

    /**
     * Shutdown the TaskExecutor and cancel any outstanding work
     */
    void shutdown() noexcept;

    /**
     * Mirror only if this maestro has been initialized
     */
    void tryMirror(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   const CommandInvocation* invocation) noexcept;

private:
    /**
     * An enum detailing the liveness of the Maestro
     *
     * The state transition map for liveness looks like so:
     * kUninitialized -> kRunning, kShutdown
     * kRunning -> kShutdown
     * kShutdown -> null
     */
    enum class Liveness {
        kUninitialized,
        kRunning,
        kShutdown,
    };

    // InitializationGuard guards and serializes the initialization and shutdown of members
    struct InitializationGuard {
        Mutex mutex = MONGO_MAKE_LATCH("MirrorMaestroImpl::InitializationGuard::mutex");
        Liveness liveness;
    };
    InitializationGuard _initGuard;

    // _isInitialized guards the use of heap allocated members like _executor
    // Even if _isInitialized is true, any member function of the variables below must still be
    // inately thread safe. If _isInitialized is false, there may not even be correct pointers to
    // call member functions upon.
    AtomicWord<bool> _isInitialized;
    std::shared_ptr<executor::TaskExecutor> _executor;
};


namespace {
constexpr auto kMirrorMaestroName = "MirrorMaestro"_sd;
constexpr auto kMirrorMaestroThreadPoolMaxThreads = 2ull;

const auto getMirrorMaestroImpl = ServiceContext::declareDecoration<MirrorMaestroImpl>();
}  // namespace

void MirroredReadsServerParameter::append(OperationContext*,
                                          BSONObjBuilder& bob,
                                          const std::string& name) {
    auto subBob = BSONObjBuilder(bob.subobjStart(name));
    _data->serialize(&subBob);
}

Status MirroredReadsServerParameter::set(const BSONElement& value) try {
    auto obj = value.Obj();

    IDLParserErrorContext ctx(name());
    _data = MirroredReadsParameters::parse(ctx, obj);

    return Status::OK();
} catch (const AssertionException& e) {
    return e.toStatus();
}

Status MirroredReadsServerParameter::setFromString(const std::string&) {
    using namespace fmt::literals;
    auto msg = "{:s} cannot be set from a string."_format(name());
    return {ErrorCodes::BadValue, msg};
}

void MirrorMaestro::init(ServiceContext* serviceContext) noexcept {
    auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
    invariant(replCoord);
    if (replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) {
        // We only need a maestro if we're in a replica set
        return;
    }

    auto& impl = getMirrorMaestroImpl(serviceContext);
    impl.init(serviceContext);
}

void MirrorMaestro::shutdown(ServiceContext* serviceContext) noexcept {
    auto& impl = getMirrorMaestroImpl(serviceContext);
    impl.shutdown();
}

void MirrorMaestro::tryMirror(OperationContext* opCtx,
                              const OpMsgRequest& request,
                              const CommandInvocation* invocation) noexcept {
    auto& impl = getMirrorMaestroImpl(opCtx->getServiceContext());
    impl.tryMirror(opCtx, request, invocation);
}

void MirrorMaestroImpl::tryMirror(OperationContext* opCtx,
                                  const OpMsgRequest& request,
                                  const CommandInvocation* invocation) noexcept {
    if (!_isInitialized.load()) {
        // If we're not even available, nothing to do
        return;
    }

    if (!invocation->supportsReadMirroring()) {
        // That's all, folks
        return;
    }

    // TODO SERVER-45816 will add the sampling function and attach the command
    repl::IsMasterResponse* imr = nullptr;
    if (!imr) {
        // If we don't have an IsMasterResponse, we can't know where to send our mirrored
        // request
        return;
    }

    MONGO_UNREACHABLE;
}

void MirrorMaestroImpl::init(ServiceContext* serviceContext) noexcept {
    LOGV2_DEBUG(31452, 2, "Initializing MirrorMaestro");

    // Until the end of this scope, no other thread can mutate _initGuard.liveness, so no other
    // thread can be in the critical section of init() or shutdown().
    stdx::lock_guard lk(_initGuard.mutex);
    switch (_initGuard.liveness) {
        case Liveness::kUninitialized: {
            // We can init
        } break;
        case Liveness::kRunning: {
            // If someone else already initialized, do nothing
            return;
        } break;
        case Liveness::kShutdown: {
            LOGV2_DEBUG(
                31453, 2, "MirrorMaestro cannot initialize as it has already been shutdown");
            return;
        } break;
    };

    auto makeNet = [&] { return executor::makeNetworkInterface(kMirrorMaestroName.toString()); };

    auto makePool = [&] {
        ThreadPool::Options options;
        options.poolName = kMirrorMaestroName.toString();
        options.maxThreads = kMirrorMaestroThreadPoolMaxThreads;
        return std::make_unique<ThreadPool>(std::move(options));
    };
    _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(makePool(), makeNet());

    _executor->startup();

    // Set _initGuard.liveness to kRunning
    _initGuard.liveness = Liveness::kRunning;

    // Mark the maestro as initialized. It is now safe to call tryMirror(), use the _executor, or
    // otherwise rely on members to be alive and well.
    _isInitialized.store(true);
}

void MirrorMaestroImpl::shutdown() noexcept {
    LOGV2_DEBUG(31454, 2, "Shutting down MirrorMaestro");

    // Until the end of this scope, no other thread can mutate _initGuard.liveness, so no other
    // thread can be in the critical section of init() or shutdown().
    stdx::lock_guard lk(_initGuard.mutex);
    switch (_initGuard.liveness) {
        case Liveness::kUninitialized:
        case Liveness::kShutdown: {
            // If someone else already shutdown or we never init'd, do nothing
            return;
        } break;
        case Liveness::kRunning: {
            // Time to shut it all down
        } break;
    };

    if (_executor) {
        _executor->shutdown();
    }

    // Set _initGuard.liveness to kShutdown
    _initGuard.liveness = Liveness::kShutdown;
}

}  // namespace mongo
