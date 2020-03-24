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
#include "mongo/bson/json.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/mirroring_sampler.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/topology_version_observer.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

namespace {
constexpr auto kMirrorMaestroName = "MirrorMaestro"_sd;
constexpr auto kMirrorMaestroThreadPoolMaxThreads = 2ull;  // Just enough to allow concurrency
constexpr auto kMirrorMaestroConnPoolMinSize = 1ull;       // Always be able to mirror eventually
constexpr auto kMirrorMaestroConnPoolMaxSize = 4ull;       // Never use more than a handful

constexpr auto kMirroredReadsParamName = "mirrorReads"_sd;

constexpr auto kMirroredReadsName = "mirroredReads"_sd;
constexpr auto kMirroredReadsSeenKey = "seen"_sd;
constexpr auto kMirroredReadsSentKey = "sent"_sd;
constexpr auto kMirroredReadsResolvedKey = "resolved"_sd;

MONGO_FAIL_POINT_DEFINE(mirrorMaestroExpectsResponse);

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
    void tryMirror(std::shared_ptr<CommandInvocation> invocation) noexcept;

private:
    /**
     * Attempt to mirror invocation to a subset of hosts based on params
     *
     * This command is expected to only run on the _executor
     */
    void _mirror(std::vector<HostAndPort> hosts,
                 std::shared_ptr<CommandInvocation> invocation,
                 MirroredReadsParameters params) noexcept;

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
    MirroredReadsServerParameter* _params = nullptr;
    MirroringSampler _sampler;
    std::shared_ptr<executor::TaskExecutor> _executor;
    repl::TopologyVersionObserver _topologyVersionObserver;
};

const auto getMirrorMaestroImpl = ServiceContext::declareDecoration<MirrorMaestroImpl>();

// Define a new serverStatus section "mirroredReads"
class MirroredReadsSection final : public ServerStatusSection {
public:
    using CounterT = long long;

    MirroredReadsSection() : ServerStatusSection(kMirroredReadsName.toString()) {}

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder section;
        section.append(kMirroredReadsSeenKey, seen.loadRelaxed());
        section.append(kMirroredReadsSentKey, sent.loadRelaxed());

        if (MONGO_unlikely(mirrorMaestroExpectsResponse.shouldFail())) {
            // We only can see if the command resolved if we got a response
            section.append(kMirroredReadsResolvedKey, resolved.loadRelaxed());
        }

        return section.obj();
    };

    AtomicWord<CounterT> seen;
    AtomicWord<CounterT> sent;
    AtomicWord<CounterT> resolved;
} gMirroredReadsSection;

auto parseMirroredReadsParameters(const BSONObj& obj) {
    IDLParserErrorContext ctx("mirrorReads");
    return MirroredReadsParameters::parse(ctx, obj);
}

}  // namespace

void MirroredReadsServerParameter::append(OperationContext*,
                                          BSONObjBuilder& bob,
                                          const std::string& name) {
    auto subBob = BSONObjBuilder(bob.subobjStart(name));
    _data->serialize(&subBob);
}

Status MirroredReadsServerParameter::set(const BSONElement& value) try {
    auto obj = value.Obj();

    _data = parseMirroredReadsParameters(obj);

    return Status::OK();
} catch (const AssertionException& e) {
    return e.toStatus();
}

Status MirroredReadsServerParameter::setFromString(const std::string& str) try {
    auto obj = fromjson(str);

    _data = parseMirroredReadsParameters(obj);

    return Status::OK();
} catch (const AssertionException& e) {
    return e.toStatus();
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

void MirrorMaestro::tryMirrorRequest(OperationContext* opCtx) noexcept {
    auto& impl = getMirrorMaestroImpl(opCtx->getServiceContext());

    auto invocation = CommandInvocation::get(opCtx);

    impl.tryMirror(std::move(invocation));
}

void MirrorMaestroImpl::tryMirror(std::shared_ptr<CommandInvocation> invocation) noexcept {
    if (!_isInitialized.load()) {
        // If we're not even available, nothing to do
        return;
    }

    invariant(invocation);
    if (!invocation->supportsReadMirroring()) {
        // That's all, folks
        return;
    }

    gMirroredReadsSection.seen.fetchAndAdd(1);

    auto params = _params->_data.get();
    auto samplingParams = MirroringSampler::SamplingParameters(params.getSamplingRate());
    if (!_sampler.shouldSample(samplingParams)) {
        // If we wouldn't select a host, then nothing more to do
        return;
    }

    auto imr = _topologyVersionObserver.getCached();
    if (!imr) {
        // If we don't have an IsMasterResponse, we can't know where to send our mirrored
        // request
        return;
    }

    auto hosts = _sampler.getRawMirroringTargets(imr);
    if (hosts.empty()) {
        // If we had no eligible hosts, nothing more to do
        return;
    }

    // There is the potential to actually mirror requests, so schedule the _mirror() invocation
    // out-of-line. This means the command itself can return quickly and we do the arduous work of
    // building new bsons and evaluating randomness in a less important context.
    ExecutorFuture(_executor)  //
        .getAsync([this,
                   hosts = std::move(hosts),
                   invocation = std::move(invocation),
                   params = std::move(params)](const auto& status) mutable {
            if (ErrorCodes::isShutdownError(status)) {
                return;
            }
            invariant(status.isOK());

            _mirror(std::move(hosts), std::move(invocation), std::move(params));
        });
}

void MirrorMaestroImpl::_mirror(std::vector<HostAndPort> hosts,
                                std::shared_ptr<CommandInvocation> invocation,
                                MirroredReadsParameters params) noexcept try {
    auto payload = [&] {
        BSONObjBuilder bob;

        invocation->appendMirrorableRequest(&bob);

        // Limit the maxTimeMS
        bob.append("maxTimeMS", params.getMaxTimeMS());

        {
            // Set secondaryPreferred read preference
            BSONObjBuilder rpBob = bob.subobjStart("$readPreference");
            rpBob.append("mode", "secondaryPreferred");
        }

        {
            // Set local read concern
            BSONObjBuilder rcBob = bob.subobjStart("readConcern");
            rcBob.append("level", "local");
        }
        return bob.obj();
    }();

    // TODO SERVER-46514 When we figure out how to deallocate CommandInvocations effectively, we
    // should do a normalized sample here.
    for (auto& host : hosts) {
        auto mirrorResponseCallback = [host](auto& args) {
            if (MONGO_likely(!mirrorMaestroExpectsResponse.shouldFail())) {
                // If we don't expect responses, then there is nothing to do here
                return;
            }

            if (MONGO_unlikely(!args.response.isOK())) {
                LOGV2_FATAL(4717301,
                            "Received mirroring response with a non-okay status",
                            "error"_attr = args.response);
            }

            gMirroredReadsSection.resolved.fetchAndAdd(1);
            LOGV2_DEBUG(
                31457, 4, "Response received", "host"_attr = host, "response"_attr = args.response);
        };

        auto newRequest = executor::RemoteCommandRequest(
            host, invocation->ns().db().toString(), payload, nullptr);
        if (MONGO_likely(!mirrorMaestroExpectsResponse.shouldFail())) {
            // If we're not expecting a response, set to fire and forget
            newRequest.fireAndForgetMode = executor::RemoteCommandRequest::FireAndForgetMode::kOn;
        }

        LOGV2_DEBUG(31455, 4, "About to mirror", "host"_attr = host, "request"_attr = newRequest);

        auto status =
            _executor->scheduleRemoteCommand(newRequest, std::move(mirrorResponseCallback))
                .getStatus();
        uassertStatusOK(status);

        gMirroredReadsSection.sent.fetchAndAdd(1);
    }
} catch (const DBException& e) {
    // TODO SERVER-44570 Invariant this only in testing
    LOGV2_DEBUG(31456, 2, "Mirroring failed", "reason"_attr = e);
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
            LOGV2_DEBUG(31453, 2, "Cannot initialize an already shutdown MirrorMaestro");
            return;
        } break;
    };

    auto makeNet = [&] {
        executor::ConnectionPool::Options options;
        options.minConnections = kMirrorMaestroConnPoolMinSize;
        options.maxConnections = kMirrorMaestroConnPoolMaxSize;
        return executor::makeNetworkInterface(
            kMirrorMaestroName.toString(), {}, {}, std::move(options));
    };

    auto makePool = [&] {
        ThreadPool::Options options;
        options.poolName = kMirrorMaestroName.toString();
        options.maxThreads = kMirrorMaestroThreadPoolMaxThreads;
        return std::make_unique<ThreadPool>(std::move(options));
    };
    _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(makePool(), makeNet());

    _executor->startup();
    _topologyVersionObserver.init(serviceContext);

    _params =
        ServerParameterSet::getGlobal()->get<MirroredReadsServerParameter>(kMirroredReadsParamName);
    invariant(_params);

    // Set _initGuard.liveness to kRunning
    _initGuard.liveness = Liveness::kRunning;

    // Mark the maestro as initialized. It is now safe to call tryMirrorRequest(), use the
    // _executor, or otherwise rely on members to be alive and well.
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

    _topologyVersionObserver.shutdown();

    if (_executor) {
        _executor->shutdown();
    }

    // Set _initGuard.liveness to kShutdown
    _initGuard.liveness = Liveness::kShutdown;
}

}  // namespace mongo
