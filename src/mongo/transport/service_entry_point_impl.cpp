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


#include "mongo/transport/service_entry_point_impl.h"

#include <boost/optional.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/variant.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_impl_gen.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_fixed.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_workflow.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/cidr.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

using namespace fmt::literals;

namespace {
bool quiet() {
    return serverGlobalParams.quiet.load();
}

/** Some diagnostic data that we will want to log about a Client after its death. */
struct ClientSummary {
    explicit ClientSummary(const Client* c)
        : uuid{c->getUUID()}, remote{c->session()->remote()}, id{c->session()->id()} {}

    friend auto logAttrs(const ClientSummary& m) {
        return logv2::multipleAttrs(
            "remote"_attr = m.remote, "uuid"_attr = m.uuid, "connectionId"_attr = m.id);
    }

    UUID uuid;
    HostAndPort remote;
    transport::SessionId id;
};
}  // namespace

bool shouldOverrideMaxConns(const std::shared_ptr<transport::Session>& session,
                            const std::vector<stdx::variant<CIDR, std::string>>& exemptions) {
    if (exemptions.empty())
        return false;

    boost::optional<CIDR> remoteCIDR;
    if (const auto& ra = session->remoteAddr(); ra.isValid() && ra.isIP())
        remoteCIDR = uassertStatusOK(CIDR::parse(ra.getAddr()));

#ifndef _WIN32
    boost::optional<std::string> localPath;
    if (const auto& la = session->localAddr(); la.isValid())
        localPath = la.getAddr();
#endif

    return std::any_of(exemptions.begin(), exemptions.end(), [&](const auto& exemption) {
        return stdx::visit(
            [&](auto&& ex) {
                using Alt = std::decay_t<decltype(ex)>;
                if constexpr (std::is_same_v<Alt, CIDR>)
                    return remoteCIDR && ex.contains(*remoteCIDR);
#ifndef _WIN32
                // Otherwise the exemption is a UNIX path and we should check the local path
                // (the remoteAddr == "anonymous unix socket") against the exemption string.
                // On Windows we don't check this at all and only CIDR ranges are supported.
                if constexpr (std::is_same_v<Alt, std::string>)
                    return localPath && *localPath == ex;
#endif
                return false;
            },
            exemption);
    });
}

size_t getSupportedMax() {
    const auto supportedMax = [] {
#ifdef _WIN32
        return serverGlobalParams.maxConns;
#else
        struct rlimit limit;
        verify(getrlimit(RLIMIT_NOFILE, &limit) == 0);

        size_t max = (size_t)(limit.rlim_cur * .8);

        LOGV2_DEBUG(22940,
                    1,
                    "fd limit hard:{hard} soft:{soft} max conn: {conn}",
                    "file descriptor and connection resource limits",
                    "hard"_attr = limit.rlim_max,
                    "soft"_attr = limit.rlim_cur,
                    "conn"_attr = max);

        return std::min(max, serverGlobalParams.maxConns);
#endif
    }();

    // If we asked for more connections than supported, inform the user.
    if (supportedMax < serverGlobalParams.maxConns &&
        serverGlobalParams.maxConns != DEFAULT_MAX_CONN) {
        LOGV2(22941,
              " --maxConns too high, can only handle {limit}",
              " --maxConns too high",
              "limit"_attr = supportedMax);
    }

    return supportedMax;
}

class ServiceEntryPointImpl::Sessions {
public:
    struct Entry {
        explicit Entry(std::shared_ptr<transport::SessionWorkflow> workflow)
            : workflow{std::move(workflow)} {}
        std::shared_ptr<transport::SessionWorkflow> workflow;
        ClientSummary summary{workflow->client()};
    };
    using ByClientMap = stdx::unordered_map<Client*, Entry>;
    using iterator = ByClientMap::iterator;

    /** A proxy object providing properly synchronized Sessions accessors. */
    class SyncToken {
    public:
        explicit SyncToken(Sessions* src) : _src{src}, _lk{_src->_mutex} {}

        /** Run `f(workflow)` for each `SessionWorkflow& workflow`, in an unspecified order. */
        template <typename F>
        void forEach(F&& f) {
            for (auto& e : _src->_byClient)
                f(*e.second.workflow);
        }

        /**
         * Waits for Sessions to drain, possibly unlocking and relocking its
         * Mutex. SyncToken holds exclusive access to a Sessions object before
         * and after this function call, but not during.
         */
        bool waitForEmpty(Date_t deadline) {
            return _src->_cv.wait_until(
                _lk, deadline.toSystemTimePoint(), [&] { return _src->_byClient.empty(); });
        }

        iterator insert(std::shared_ptr<transport::SessionWorkflow> workflow) {
            Client* cli = workflow->client();
            auto [it, ok] = _src->_byClient.insert({cli, Entry(std::move(workflow))});
            invariant(ok);
            _src->_created.fetchAndAdd(1);
            _onSizeChange();
            return it;
        }

        void erase(iterator it) {
            _src->_byClient.erase(it);
            _onSizeChange();
        }

        iterator find(Client* client) {
            auto iter = _src->_byClient.find(client);
            invariant(iter != _src->_byClient.end());
            return iter;
        }

        size_t size() const {
            return _src->_byClient.size();
        }

    private:
        void _onSizeChange() {
            _src->_size.store(_src->_byClient.size());
            _src->_cv.notify_all();
        }

        Sessions* _src;
        stdx::unique_lock<Mutex> _lk;
    };

    /** Returns a proxy object providing synchronized mutable access to the Sessions object. */
    SyncToken sync() {
        return SyncToken(this);
    }

    size_t size() const {
        return _size.load();
    }

    size_t created() const {
        return _created.load();
    }

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ServiceEntryPointImpl::Sessions::_mutex");
    stdx::condition_variable _cv;    ///< notified on `_byClient` changes.
    AtomicWord<size_t> _size{0};     ///< Kept in sync with `_byClient.size()`
    AtomicWord<size_t> _created{0};  ///< Increases with each `insert` call.
    ByClientMap _byClient;           ///< guarded by `_mutex`
};

ServiceEntryPointImpl::ServiceEntryPointImpl(ServiceContext* svcCtx)
    : _svcCtx(svcCtx),
      _maxSessions(getSupportedMax()),
      _rejectedSessions(0),
      _sessions{std::make_unique<Sessions>()} {}

ServiceEntryPointImpl::~ServiceEntryPointImpl() = default;

Status ServiceEntryPointImpl::start() {
    if (auto status = transport::ServiceExecutorSynchronous::get(_svcCtx)->start();
        !status.isOK()) {
        return status;
    }

    if (auto exec = transport::ServiceExecutorReserved::get(_svcCtx)) {
        if (auto status = exec->start(); !status.isOK()) {
            return status;
        }
    }

    if (auto status = transport::ServiceExecutorFixed::get(_svcCtx)->start(); !status.isOK()) {
        return status;
    }

    return Status::OK();
}

void ServiceEntryPointImpl::configureServiceExecutorContext(ServiceContext::UniqueClient& client,
                                                            bool isPrivilegedSession) {
    auto seCtx = std::make_unique<transport::ServiceExecutorContext>();
    seCtx->setUseDedicatedThread(transport::gInitialUseDedicatedThread);
    seCtx->setCanUseReserved(isPrivilegedSession);
    stdx::lock_guard lk(*client);
    transport::ServiceExecutorContext::set(&*client, std::move(seCtx));
}

void ServiceEntryPointImpl::startSession(std::shared_ptr<transport::Session> session) {
    invariant(session);

    transport::IngressHandshakeMetrics::get(*session).onSessionStarted(_svcCtx->getTickSource());

    // Setup the restriction environment on the Session, if the Session has local/remote Sockaddrs
    const auto& remoteAddr = session->remoteAddr();
    const auto& localAddr = session->localAddr();
    invariant(remoteAddr.isValid() && localAddr.isValid());
    auto restrictionEnvironment = std::make_unique<RestrictionEnvironment>(remoteAddr, localAddr);
    RestrictionEnvironment::set(session, std::move(restrictionEnvironment));

    bool isPrivilegedSession = shouldOverrideMaxConns(session, serverGlobalParams.maxConnsOverride);

    auto client = _svcCtx->makeClient("conn{}"_format(session->id()), session);
    auto clientPtr = client.get();

    std::shared_ptr<transport::SessionWorkflow> workflow;
    {
        auto sync = _sessions->sync();
        if (sync.size() >= _maxSessions && !isPrivilegedSession) {
            // Since startSession() is guaranteed to be accessed only by a single listener thread,
            // an atomic increment is not necessary here.
            _rejectedSessions++;
            if (!quiet()) {
                LOGV2(22942,
                      "Connection refused because there are too many open connections",
                      "remote"_attr = session->remote(),
                      "connectionCount"_attr = sync.size());
            }
            return;
        }

        configureServiceExecutorContext(client, isPrivilegedSession);

        workflow = transport::SessionWorkflow::make(std::move(client));
        auto iter = sync.insert(workflow);
        if (!quiet()) {
            LOGV2(22943,
                  "Connection accepted",
                  logAttrs(iter->second.summary),
                  "connectionCount"_attr = sync.size());
        }
    }

    onClientConnect(clientPtr);
    workflow->start();
}

void ServiceEntryPointImpl::onClientDisconnect(Client* client) {
    derivedOnClientDisconnect(client);
    {
        stdx::lock_guard lk(*client);
        transport::ServiceExecutorContext::reset(client);
    }
    auto sync = _sessions->sync();
    auto iter = sync.find(client);
    auto summary = iter->second.summary;
    sync.erase(iter);
    if (!quiet()) {
        LOGV2(22944, "Connection ended", logAttrs(summary), "connectionCount"_attr = sync.size());
    }
}

void ServiceEntryPointImpl::endAllSessions(transport::Session::TagMask tags) {
    _sessions->sync().forEach([&](auto&& workflow) { workflow.terminateIfTagsDontMatch(tags); });
}

bool ServiceEntryPointImpl::shutdown(Milliseconds timeout) {
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    static constexpr bool kSanitizerBuild = true;
#else
    static constexpr bool kSanitizerBuild = false;
#endif

    // When running under address sanitizer, we get false positive leaks due to disorder around
    // the lifecycle of a connection and request. When we are running under ASAN, we try a lot
    // harder to dry up the server from active connections before going on to really shut down.
    // In non-sanitizer builds, a feature flag can enable a true shutdown anyway. We use the
    // flag to identify these shutdown problems in testing.
    if (kSanitizerBuild || transport::gJoinIngressSessionsOnShutdown) {
        const auto result = shutdownAndWait(timeout);
        if (transport::gJoinIngressSessionsOnShutdown)
            invariant(result, "Shutdown did not complete within {}ms"_format(timeout.count()));
        return result;
    }

    return true;
}

size_t ServiceEntryPointImpl::numOpenSessions() const {
    return _sessions->size();
}

size_t ServiceEntryPointImpl::maxOpenSessions() const {
    return _maxSessions;
}

logv2::LogSeverity ServiceEntryPointImpl::slowSessionWorkflowLogSeverity() {
    return _slowSessionWorkflowLogSuppressor();
}

bool ServiceEntryPointImpl::shutdownAndWait(Milliseconds timeout) {
    auto deadline = _svcCtx->getPreciseClockSource()->now() + timeout;

    // Issue a terminate to all sessions, then wait for them to drain.
    // If there are undrained sessions after the deadline, shutdown continues.
    LOGV2(6367401, "Shutting down service entry point and waiting for sessions to join");

    bool drainedAll;
    {
        auto sync = _sessions->sync();
        sync.forEach([&](auto&& workflow) { workflow.terminate(); });
        drainedAll = sync.waitForEmpty(deadline);
        if (!drainedAll) {
            LOGV2(22947,
                  "Shutdown: some sessions not drained after deadline. Continuing shutdown",
                  "sessions"_attr = sync.size());
        } else {
            LOGV2(22946, "Shutdown: all sessions drained");
        }
    }

    transport::ServiceExecutor::shutdownAll(_svcCtx, deadline);

    return drainedAll;
}

void ServiceEntryPointImpl::endAllSessionsNoTagMask() {
    _sessions->sync().forEach([&](auto&& workflow) { workflow.terminate(); });
}

bool ServiceEntryPointImpl::waitForNoSessions(Milliseconds timeout) {
    auto deadline = _svcCtx->getPreciseClockSource()->now() + timeout;
    LOGV2(5342100, "Waiting for all sessions to conclude", "deadline"_attr = deadline);

    return _sessions->sync().waitForEmpty(deadline);
}

void ServiceEntryPointImpl::appendStats(BSONObjBuilder* bob) const {
    size_t sessionCount = _sessions->size();
    size_t sessionsCreated = _sessions->created();

    auto appendInt = [&](StringData n, auto v) {
        bob->append(n, static_cast<int>(v));
    };

    appendInt("current", sessionCount);
    appendInt("available", _maxSessions - sessionCount);
    appendInt("totalCreated", sessionsCreated);

    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe()) {
        appendInt("rejected", _rejectedSessions);
    }

    invariant(_svcCtx);
    appendInt("active", _svcCtx->getActiveClientOperations());

    const auto seStats = transport::ServiceExecutorStats::get(_svcCtx);
    appendInt("threaded", seStats.usesDedicated);
    if (!serverGlobalParams.maxConnsOverride.empty())
        appendInt("limitExempt", seStats.limitExempt);

    auto&& hm = HelloMetrics::get(_svcCtx);
    appendInt("exhaustIsMaster", hm->getNumExhaustIsMaster());
    appendInt("exhaustHello", hm->getNumExhaustHello());
    appendInt("awaitingTopologyChanges", hm->getNumAwaitingTopologyChanges());

    if (auto adminExec = transport::ServiceExecutorReserved::get(_svcCtx)) {
        BSONObjBuilder section(bob->subobjStart("adminConnections"));
        adminExec->appendStats(&section);
    }
}

}  // namespace mongo
