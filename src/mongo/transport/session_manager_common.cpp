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

#include "mongo/transport/session_manager_common.h"

#include <boost/optional.hpp>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager_common_gen.h"
#include "mongo/transport/session_workflow.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {
namespace {

/** Some diagnostic data that we will want to log about a Client after its death. */
struct ClientSummary {
    explicit ClientSummary(const Client* c)
        : uuid(c->getUUID()), remote(c->session()->remote()), id(c->session()->id()) {}

    friend auto logAttrs(const ClientSummary& m) {
        return logv2::multipleAttrs(
            "remote"_attr = m.remote, "uuid"_attr = m.uuid, "connectionId"_attr = m.id);
    }

    UUID uuid;
    HostAndPort remote;
    SessionId id;
};

bool quiet() {
    return serverGlobalParams.quiet.load();
}


/**
 * Limit maximum sessions to `serverGlobalParams.maxConns`.
 *
 * Furthermore, on non-Windows, the max number of open connections is automatically
 * capped to 80% of half the open file limit. Half to account for the two file
 * descriptors for each open connection (a second file descriptor is needed to make clients
 * notifable through eventfd) and 80% to pad for descriptors unrelated to connections, such as for
 * collections.
 */
std::size_t getSupportedMax() {
    const auto supportedMax = ([] {
#ifdef _WIN32
        return serverGlobalParams.maxConns;
#else
        struct rlimit limit;
        MONGO_verify(getrlimit(RLIMIT_NOFILE, &limit) == 0);
        const auto openFileLimit = limit.rlim_cur / 2;
        const auto max = static_cast<std::size_t>(openFileLimit * .8);

        LOGV2_DEBUG(22940,
                    1,
                    "file descriptor and connection resource limits",
                    "hard"_attr = limit.rlim_max,
                    "soft"_attr = limit.rlim_cur,
                    "conn"_attr = max);

        return std::min(max, serverGlobalParams.maxConns);
#endif
    })();

    // If we asked for more connections than supported, inform the user.
    if (supportedMax < serverGlobalParams.maxConns &&
        serverGlobalParams.maxConns != DEFAULT_MAX_CONN) {
        LOGV2(22941, " --maxConns too high", "limit"_attr = supportedMax);
    }

    return supportedMax;
}

}  // namespace

/**
 * Container implementation for currently active sessions.
 * Structurally this behaves like an STL map<Client*, SessionWorkflow*>
 * with additional machinery to manage concurrency.
 */
class SessionManagerCommon::Sessions {
public:
    struct Entry {
        explicit Entry(std::shared_ptr<SessionWorkflow> workflow) : workflow{std::move(workflow)} {}
        std::shared_ptr<SessionWorkflow> workflow;
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
            for (auto& e : _src->_byClient) {
                f(*e.second.workflow);
            }
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

        iterator find(Client* client) const {
            auto iter = _src->_byClient.find(client);
            invariant(iter != _src->_byClient.end());
            return iter;
        }

        std::size_t size() const {
            return _src->_byClient.size();
        }

        std::size_t created() const {
            return _src->_created.load();
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

    std::size_t size() const {
        return _size.load();
    }

    std::size_t created() const {
        return _created.load();
    }

    void incrementRejected() {
        _rejected.fetchAndAdd(1);
    }

    std::size_t rejected() const {
        return _rejected.load();
    }

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ServiceEntryPoint::Sessions::_mutex");
    stdx::condition_variable _cv;      ///< notified on `_byClient` changes.
    Atomic<std::size_t> _size{0};      ///< Kept in sync with `_byClient.size()`
    Atomic<std::size_t> _created{0};   ///< Increases with each `insert` call.
    Atomic<std::size_t> _rejected{0};  ///< Used to record when a session is rejected.
    ByClientMap _byClient;             ///< guarded by `_mutex`
};

SessionManagerCommon::SessionManagerCommon(ServiceContext* svcCtx)
    : SessionManagerCommon(svcCtx, std::vector<std::shared_ptr<ClientTransportObserver>>()) {}

// Helper for single observer constructor.
// std::initializer_list uses copy semantics, so we can't just call the vector version with:
// `{std::make_unique<MyObserver>()}`.
// Instead, construct with an empty array then push our singular one in.
SessionManagerCommon::SessionManagerCommon(ServiceContext* svcCtx,
                                           std::shared_ptr<ClientTransportObserver> observer)
    : SessionManagerCommon(svcCtx) {
    invariant(observer);
    _observers.push_back(std::move(observer));
}

SessionManagerCommon::SessionManagerCommon(
    ServiceContext* svcCtx, std::vector<std::shared_ptr<ClientTransportObserver>> observers)
    : _svcCtx(svcCtx),
      _maxOpenSessions(getSupportedMax()),
      _sessions(std::make_unique<Sessions>()),
      _observers(std::move(observers)) {}

SessionManagerCommon::~SessionManagerCommon() = default;

void SessionManagerCommon::startSession(std::shared_ptr<Session> session) {
    invariant(session);
    IngressHandshakeMetrics::get(*session).onSessionStarted(_svcCtx->getTickSource());

    const bool isPrivilegedSession =
        session->shouldOverrideMaxConns(serverGlobalParams.maxConnsOverride);
    const bool verbose = !quiet();

    auto service = _svcCtx->getService();
    // Serverless clusters don't support sharding, so they should only ever use the Shard service
    // and associated ServiceEntryPoint.
    if (!gMultitenancySupport && session->isFromRouterPort()) {
        service = _svcCtx->getService(ClusterRole::RouterServer);
    }

    auto uniqueClient = service->makeClient(getClientThreadName(*session), session);
    auto client = uniqueClient.get();

    std::shared_ptr<transport::SessionWorkflow> workflow;
    {
        auto sync = _sessions->sync();
        if (sync.size() >= _maxOpenSessions && !isPrivilegedSession) {
            _sessions->incrementRejected();
            if (verbose) {
                LOGV2(22942,
                      "Connection refused because there are too many open connections",
                      "remote"_attr = session->remote(),
                      "connectionCount"_attr = sync.size());
            }
            session->end();
            return;
        }

        configureServiceExecutorContext(client, isPrivilegedSession);

        workflow = SessionWorkflow::make(std::move(uniqueClient));
        auto iter = sync.insert(workflow);
        if (verbose) {
            LOGV2(22943,
                  "Connection accepted",
                  logAttrs(iter->second.summary),
                  "connectionCount"_attr = sync.size());
        }
    }

    onClientConnect(client);
    for (auto&& observer : _observers) {
        observer->onClientConnect(client);
    }

    workflow->start();
}

void SessionManagerCommon::endAllSessions(Client::TagMask tags) {
    _sessions->sync().forEach([&](auto&& workflow) { workflow.terminateIfTagsDontMatch(tags); });
}

void SessionManagerCommon::endAllSessionsNoTagMask() {
    _sessions->sync().forEach([&](auto&& workflow) { workflow.terminate(); });
}

bool SessionManagerCommon::shutdown(Milliseconds timeout) {
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
    using namespace fmt::literals;
    if (kSanitizerBuild || gJoinIngressSessionsOnShutdown) {
        const auto result = shutdownAndWait(timeout);
        invariant(result || !gJoinIngressSessionsOnShutdown,
                  "Shutdown did not complete within {}ms"_format(timeout.count()));
        return result;
    }

    return true;
}
bool SessionManagerCommon::shutdownAndWait(Milliseconds timeout) {
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

    return drainedAll;
}

bool SessionManagerCommon::waitForNoSessions(Milliseconds timeout) {
    auto deadline = _svcCtx->getPreciseClockSource()->now() + timeout;
    LOGV2(5342100, "Waiting for all sessions to conclude", "deadline"_attr = deadline);

    return _sessions->sync().waitForEmpty(deadline);
}

std::size_t SessionManagerCommon::numOpenSessions() const {
    auto sync = _sessions->sync();
    return sync.size();
}

std::size_t SessionManagerCommon::numCreatedSessions() const {
    return _sessions->created();
}

std::size_t SessionManagerCommon::numRejectedSessions() const {
    return _sessions->rejected();
}

void SessionManagerCommon::endSessionByClient(Client* client) {
    onClientDisconnect(client);
    for (auto&& observer : _observers) {
        observer->onClientDisconnect(client);
    }

    {
        stdx::lock_guard lk(*client);
        ServiceExecutorContext::reset(client);
    }
    auto sync = _sessions->sync();
    auto iter = sync.find(client);
    auto summary = iter->second.summary;
    iter->second.workflow->terminate();
    sync.erase(iter);
    if (!quiet()) {
        LOGV2(22944, "Connection ended", logAttrs(summary), "connectionCount"_attr = sync.size());
    }
}

}  // namespace mongo::transport
