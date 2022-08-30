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


#include "mongo/platform/basic.h"

#include "mongo/db/mirror_maestro.h"

#include <cmath>
#include <cstdlib>
#include <utility>

#include <fmt/format.h>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/client_out_of_line_executor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/mirroring_sampler.h"
#include "mongo/db/repl/hello_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/topology_version_observer.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {
constexpr auto kMirrorMaestroName = "MirrorMaestro"_sd;
constexpr auto kMirrorMaestroThreadPoolMaxThreads = 2ull;  // Just enough to allow concurrency
constexpr auto kMirrorMaestroConnPoolMinSize = 1ull;       // Always be able to mirror eventually
constexpr auto kMirrorMaestroConnPoolMaxSize = 4ull;       // Never use more than a handful

constexpr auto kMirroredReadsParamName = "mirrorReads"_sd;

constexpr auto kMirroredReadsSeenKey = "seen"_sd;
constexpr auto kMirroredReadsSentKey = "sent"_sd;
constexpr auto kMirroredReadsReceivedKey = "received"_sd;
constexpr auto kMirroredReadsResolvedKey = "resolved"_sd;
constexpr auto kMirroredReadsResolvedBreakdownKey = "resolvedBreakdown"_sd;
constexpr auto kMirroredReadsPendingKey = "pending"_sd;

MONGO_FAIL_POINT_DEFINE(mirrorMaestroExpectsResponse);
MONGO_FAIL_POINT_DEFINE(mirrorMaestroTracksPending);

class MirrorMaestroImpl {
public:
    /**
     * Make the TaskExecutor and initialize other components.
     */
    void init(ServiceContext* serviceContext) noexcept;

    /**
     * Shutdown the TaskExecutor and cancel any outstanding work.
     */
    void shutdown() noexcept;

    /**
     * Mirror only if this maestro has been initialized.
     */
    void tryMirror(std::shared_ptr<CommandInvocation> invocation) noexcept;

    /**
     * Maintains the state required for mirroring requests.
     */
    class MirroredRequestState {
    public:
        MirroredRequestState(MirrorMaestroImpl* maestro,
                             std::vector<HostAndPort> hosts,
                             std::shared_ptr<CommandInvocation> invocation,
                             MirroredReadsParameters params)
            : _maestro(std::move(maestro)),
              _hosts(std::move(hosts)),
              _invocation(std::move(invocation)),
              _params(std::move(params)) {}

        MirroredRequestState() = delete;

        void mirror() noexcept {
            invariant(_maestro);
            _maestro->_mirror(_hosts, _invocation, _params);
        }

    private:
        MirrorMaestroImpl* _maestro;
        std::vector<HostAndPort> _hosts;
        std::shared_ptr<CommandInvocation> _invocation;
        MirroredReadsParameters _params;
    };

private:
    /**
     * Attempt to mirror invocation to a subset of hosts based on params
     *
     * This command is expected to only run on the _executor
     */
    void _mirror(const std::vector<HostAndPort>& hosts,
                 std::shared_ptr<CommandInvocation> invocation,
                 const MirroredReadsParameters& params) noexcept;

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

    MirroredReadsSection()
        : ServerStatusSection(MirrorMaestro::kServerStatusSectionName.toString()) {}

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder section;
        section.append(kMirroredReadsSeenKey, seen.loadRelaxed());
        section.append(kMirroredReadsSentKey, sent.loadRelaxed());
        section.append(kMirroredReadsReceivedKey, received.loadRelaxed());

        if (MONGO_unlikely(mirrorMaestroExpectsResponse.shouldFail())) {
            // We only can see if the command resolved if we got a response
            section.append(kMirroredReadsResolvedKey, resolved.loadRelaxed());
            section.append(kMirroredReadsResolvedBreakdownKey, resolvedBreakdown.toBSON());
        }
        if (MONGO_unlikely(mirrorMaestroTracksPending.shouldFail())) {
            section.append(kMirroredReadsPendingKey, pending.loadRelaxed());
        }
        return section.obj();
    };

    /**
     * Maintains a breakdown for resolved requests by host name.
     * This class may only be used for testing (e.g., as part of a fail-point).
     */
    class ResolvedBreakdownByHost {
    public:
        void onResponseReceived(const HostAndPort& host) noexcept {
            const auto hostName = host.toString();
            stdx::lock_guard<Mutex> lk(_mutex);

            if (_resolved.find(hostName) == _resolved.end()) {
                _resolved[hostName] = 0;
            }

            _resolved[hostName]++;
        }

        BSONObj toBSON() const noexcept {
            stdx::lock_guard<Mutex> lk(_mutex);
            BSONObjBuilder bob;
            for (auto entry : _resolved) {
                bob.append(entry.first, entry.second);
            }
            return bob.obj();
        }

    private:
        mutable Mutex _mutex = MONGO_MAKE_LATCH("ResolvedBreakdownByHost"_sd);

        stdx::unordered_map<std::string, CounterT> _resolved;
    };

    ResolvedBreakdownByHost resolvedBreakdown;

    AtomicWord<CounterT> seen;
    AtomicWord<CounterT> sent;
    AtomicWord<CounterT> resolved;
    // Counts the number of operations that are scheduled to be mirrored, but haven't yet been sent.
    AtomicWord<CounterT> pending;
    // Counts the number of mirrored operations received by this node as a secondary.
    AtomicWord<CounterT> received;
} gMirroredReadsSection;

auto parseMirroredReadsParameters(const BSONObj& obj) {
    IDLParserContext ctx("mirrorReads");
    return MirroredReadsParameters::parse(ctx, obj);
}

}  // namespace

void MirroredReadsServerParameter::append(OperationContext*,
                                          BSONObjBuilder* bob,
                                          StringData name,
                                          const boost::optional<TenantId>&) {
    auto subBob = BSONObjBuilder(bob->subobjStart(name));
    _data->serialize(&subBob);
}

Status MirroredReadsServerParameter::set(const BSONElement& value,
                                         const boost::optional<TenantId>&) try {
    auto obj = value.Obj();

    _data = parseMirroredReadsParameters(obj);

    return Status::OK();
} catch (const AssertionException& e) {
    return e.toStatus();
}

Status MirroredReadsServerParameter::setFromString(StringData str,
                                                   const boost::optional<TenantId>&) try {
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

void MirrorMaestro::onReceiveMirroredRead(OperationContext* opCtx) noexcept {
    const auto& invocation = CommandInvocation::get(opCtx);
    if (MONGO_unlikely(invocation->isMirrored())) {
        gMirroredReadsSection.received.fetchAndAddRelaxed(1);
    }
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
    if (params.getSamplingRate() == 0) {
        // Nothing to do if sampling rate is zero.
        return;
    }

    auto imr = _topologyVersionObserver.getCached();
    auto samplingParams = MirroringSampler::SamplingParameters(params.getSamplingRate());
    if (!_sampler.shouldSample(imr, samplingParams)) {
        // If we wouldn't select a host, then nothing more to do
        return;
    }

    auto hosts = _sampler.getRawMirroringTargets(imr);
    invariant(!hosts.empty());

    auto clientExecutor = ClientOutOfLineExecutor::get(Client::getCurrent());
    auto clientExecutorHandle = clientExecutor->getHandle();

    // NOTE: before using Client's out-of-line executor outside of MirrorMaestro, we must first
    // move the consumption (i.e., `consumeAllTasks`) to the baton.
    clientExecutor->consumeAllTasks();

    // There is the potential to actually mirror requests, so schedule the _mirror() invocation
    // out-of-line. This means the command itself can return quickly and we do the arduous work of
    // building new bsons and evaluating randomness in a less important context.
    auto requestState = std::make_unique<MirroredRequestState>(
        this, std::move(hosts), std::move(invocation), std::move(params));
    if (MONGO_unlikely(mirrorMaestroTracksPending.shouldFail())) {
        // We've scheduled the operation to be mirrored; it is now "pending" until it has actually
        // been sent to a secondary.
        gMirroredReadsSection.pending.fetchAndAdd(1);
    }
    ExecutorFuture(_executor)  //
        .getAsync([clientExecutorHandle,
                   requestState = std::move(requestState)](const auto& status) mutable {
            ON_BLOCK_EXIT([&] {
                if (MONGO_unlikely(mirrorMaestroTracksPending.shouldFail())) {
                    // The read has been sent to at least one secondary, so it's no longer pending
                    gMirroredReadsSection.pending.fetchAndSubtract(1);
                }
            });
            if (!ErrorCodes::isShutdownError(status)) {
                invariant(status.isOK());
                requestState->mirror();
            }
            clientExecutorHandle.schedule([requestState = std::move(requestState)](
                                              const Status&) mutable { requestState.reset(); });
        });
}

void MirrorMaestroImpl::_mirror(const std::vector<HostAndPort>& hosts,
                                std::shared_ptr<CommandInvocation> invocation,
                                const MirroredReadsParameters& params) noexcept try {
    auto payload = [&] {
        BSONObjBuilder bob;

        invocation->appendMirrorableRequest(&bob);

        // Limit the maxTimeMS
        bob.append("maxTimeMS", params.getMaxTimeMS());

        // Indicate that this is a mirrored read.
        bob.append("mirrored", true);

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

    // Mirror to a normalized subset of eligible hosts (i.e., secondaries).
    const auto startIndex = rand() % hosts.size();
    const auto mirroringFactor = std::ceil(params.getSamplingRate() * hosts.size());

    for (auto i = 0; i < mirroringFactor; i++) {
        auto& host = hosts[(startIndex + i) % hosts.size()];
        auto mirrorResponseCallback = [host](auto& args) {
            if (MONGO_likely(!mirrorMaestroExpectsResponse.shouldFail())) {
                // If we don't expect responses, then there is nothing to do here
                return;
            }

            // Count both failed and successful reads as resolved
            gMirroredReadsSection.resolved.fetchAndAdd(1);
            gMirroredReadsSection.resolvedBreakdown.onResponseReceived(host);
            LOGV2_DEBUG(
                31457, 4, "Response received", "host"_attr = host, "response"_attr = args.response);

            if (ErrorCodes::isRetriableError(args.response.status)) {
                LOGV2_WARNING(5089200,
                              "Received mirroring response with a retriable failure",
                              "error"_attr = args.response);
                return;
            } else if (!args.response.isOK()) {
                LOGV2_FATAL(4717301,
                            "Received mirroring response with a non-okay status",
                            "error"_attr = args.response);
            }
        };

        auto newRequest = executor::RemoteCommandRequest(
            host, invocation->ns().db().toString(), payload, nullptr);

        newRequest.options.fireAndForget = true;
        if (MONGO_unlikely(mirrorMaestroExpectsResponse.shouldFail()))
            newRequest.options.fireAndForget = false;

        LOGV2_DEBUG(31455, 4, "About to mirror", "host"_attr = host, "request"_attr = newRequest);

        auto status =
            _executor->scheduleRemoteCommand(newRequest, std::move(mirrorResponseCallback))
                .getStatus();

        if (ErrorCodes::isShutdownError(status.code())) {
            LOGV2_DEBUG(5723501, 1, "Aborted mirroring due to shutdown", "reason"_attr = status);
            return;
        }

        tassert(status);
        gMirroredReadsSection.sent.fetchAndAdd(1);
    }
} catch (const DBException& e) {
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

    _params = ServerParameterSet::getNodeParameterSet()->get<MirroredReadsServerParameter>(
        kMirroredReadsParamName);
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
