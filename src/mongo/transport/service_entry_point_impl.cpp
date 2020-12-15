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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/service_entry_point_impl.h"

#include <fmt/format.h>
#include <vector>

#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_gen.h"
#include "mongo/transport/service_state_machine.h"
#include "mongo/transport/session.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

using namespace fmt::literals;

bool shouldOverrideMaxConns(const transport::SessionHandle& session,
                            const std::vector<stdx::variant<CIDR, std::string>>& exemptions) {
    if (exemptions.empty()) {
        return false;
    }

    const auto& remoteAddr = session->remoteAddr();
    const auto& localAddr = session->localAddr();

    boost::optional<CIDR> remoteCIDR;

    if (remoteAddr.isValid() && remoteAddr.isIP()) {
        remoteCIDR = uassertStatusOK(CIDR::parse(remoteAddr.getAddr()));
    }
    for (const auto& exemption : exemptions) {
        // If this exemption is a CIDR range, then we check that the remote IP is in the
        // CIDR range
        if ((stdx::holds_alternative<CIDR>(exemption)) && (remoteCIDR)) {
            if (stdx::get<CIDR>(exemption).contains(*remoteCIDR)) {
                return true;
            }
// Otherwise the exemption is a UNIX path and we should check the local path
// (the remoteAddr == "anonymous unix socket") against the exemption string
//
// On Windows we don't check this at all and only CIDR ranges are supported
#ifndef _WIN32
        } else if ((stdx::holds_alternative<std::string>(exemption)) && localAddr.isValid() &&
                   (localAddr.getAddr() == stdx::get<std::string>(exemption))) {
            return true;
#endif
        }
    }

    return false;
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

ServiceEntryPointImpl::ServiceEntryPointImpl(ServiceContext* svcCtx)
    : _svcCtx(svcCtx), _maxNumConnections(getSupportedMax()) {}

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

void ServiceEntryPointImpl::startSession(transport::SessionHandle session) {
    // Setup the restriction environment on the Session, if the Session has local/remote Sockaddrs
    const auto& remoteAddr = session->remoteAddr();
    const auto& localAddr = session->localAddr();
    invariant(remoteAddr.isValid() && localAddr.isValid());
    auto restrictionEnvironment = std::make_unique<RestrictionEnvironment>(remoteAddr, localAddr);
    RestrictionEnvironment::set(session, std::move(restrictionEnvironment));

    bool canOverrideMaxConns = shouldOverrideMaxConns(session, serverGlobalParams.maxConnsOverride);

    auto clientName = "conn{}"_format(session->id());
    auto client = _svcCtx->makeClient(clientName, session);

    const bool quiet = serverGlobalParams.quiet.load();

    size_t connectionCount;
    auto maybeSsmIt = [&]() -> boost::optional<SSMListIterator> {
        stdx::lock_guard lk(_sessionsMutex);
        connectionCount = _currentConnections.load();
        if (connectionCount > _maxNumConnections && !canOverrideMaxConns) {
            return boost::none;
        }

        auto it = _sessions.emplace(_sessions.begin(), std::move(client));
        connectionCount = _sessions.size();
        _currentConnections.store(connectionCount);
        _createdConnections.addAndFetch(1);
        return it;
    }();

    if (!maybeSsmIt) {
        if (!quiet) {
            LOGV2(22942,
                  "Connection refused because there are too many open connections",
                  "remote"_attr = session->remote(),
                  "connectionCount"_attr = connectionCount);
        }
        return;
    } else if (!quiet) {
        LOGV2(22943,
              "Connection accepted",
              "remote"_attr = session->remote(),
              "connectionId"_attr = session->id(),
              "connectionCount"_attr = connectionCount);
    }

    auto ssmIt = *maybeSsmIt;
    ssmIt->setCleanupHook([this, ssmIt, quiet, session = std::move(session)] {
        size_t connectionCount;
        auto remote = session->remote();
        {
            stdx::lock_guard<decltype(_sessionsMutex)> lk(_sessionsMutex);
            _sessions.erase(ssmIt);
            connectionCount = _sessions.size();
            _currentConnections.store(connectionCount);
        }

        if (!quiet) {
            LOGV2(22944,
                  "Connection ended",
                  "remote"_attr = remote,
                  "connectionId"_attr = session->id(),
                  "connectionCount"_attr = connectionCount);
        }

        _sessionsCV.notify_one();
    });

    auto seCtx = transport::ServiceExecutorContext{};
    seCtx.setThreadingModel(transport::ServiceExecutor::getInitialThreadingModel());
    seCtx.setCanUseReserved(canOverrideMaxConns);
    ssmIt->start(std::move(seCtx));
}

void ServiceEntryPointImpl::endAllSessions(transport::Session::TagMask tags) {
    // While holding the _sesionsMutex, loop over all the current connections, and if their tags
    // do not match the requested tags to skip, terminate the session.
    {
        stdx::unique_lock<decltype(_sessionsMutex)> lk(_sessionsMutex);
        for (auto& ssm : _sessions) {
            ssm.terminateIfTagsDontMatch(tags);
        }
    }
}

bool ServiceEntryPointImpl::shutdown(Milliseconds timeout) {
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    // When running under address sanitizer, we get false positive leaks due to disorder around
    // the lifecycle of a connection and request. When we are running under ASAN, we try a lot
    // harder to dry up the server from active connections before going on to really shut down.
    return shutdownAndWait(timeout);
#else
    return true;
#endif
}

bool ServiceEntryPointImpl::shutdownAndWait(Milliseconds timeout) {
    auto deadline = _svcCtx->getPreciseClockSource()->now() + timeout;

    stdx::unique_lock<decltype(_sessionsMutex)> lk(_sessionsMutex);

    // Request that all sessions end, while holding the _sesionsMutex, loop over all the current
    // connections and terminate them. Then wait for the number of active connections to reach zero
    // with a condition_variable that notifies in the session cleanup hook. If we haven't closed
    // drained all active operations within the deadline, just keep going with shutdown: the OS will
    // do it for us when the process terminates.
    _terminateAll(lk);
    auto result = _waitForNoSessions(lk, deadline);
    lk.unlock();

    if (result) {
        LOGV2(22946, "shutdown: no running workers found...");
    } else {
        LOGV2(
            22947,
            "shutdown: exhausted grace period for {workers} active workers to "
            "drain; continuing with shutdown...",
            "shutdown: exhausted grace period active workers to drain; continuing with shutdown...",
            "workers"_attr = numOpenSessions());
    }

    transport::ServiceExecutor::shutdownAll(_svcCtx, deadline);

    return result;
}

void ServiceEntryPointImpl::endAllSessionsNoTagMask() {
    auto lk = stdx::unique_lock<decltype(_sessionsMutex)>(_sessionsMutex);
    _terminateAll(lk);
}

void ServiceEntryPointImpl::_terminateAll(WithLock) {
    for (auto& ssm : _sessions) {
        ssm.terminate();
    }
}

bool ServiceEntryPointImpl::waitForNoSessions(Milliseconds timeout) {
    auto deadline = _svcCtx->getPreciseClockSource()->now() + timeout;
    LOGV2(5342100, "Waiting until for all sessions to conclude", "deadline"_attr = deadline);

    auto lk = stdx::unique_lock<decltype(_sessionsMutex)>(_sessionsMutex);
    return _waitForNoSessions(lk, deadline);
}

bool ServiceEntryPointImpl::_waitForNoSessions(stdx::unique_lock<decltype(_sessionsMutex)>& lk,
                                               Date_t deadline) {
    auto noWorkersLeft = [this] { return numOpenSessions() == 0; };
    _sessionsCV.wait_until(lk, deadline.toSystemTimePoint(), noWorkersLeft);

    return noWorkersLeft();
}

void ServiceEntryPointImpl::appendStats(BSONObjBuilder* bob) const {

    size_t sessionCount = _currentConnections.load();

    bob->append("current", static_cast<int>(sessionCount));
    bob->append("available", static_cast<int>(_maxNumConnections - sessionCount));
    bob->append("totalCreated", static_cast<int>(_createdConnections.load()));

    invariant(_svcCtx);
    bob->append("active", static_cast<int>(_svcCtx->getActiveClientOperations()));

    const auto seStats = transport::ServiceExecutorStats::get(_svcCtx);
    bob->append("threaded", static_cast<int>(seStats.usesDedicated));
    if (serverGlobalParams.maxConnsOverride.size()) {
        bob->append("limitExempt", static_cast<int>(seStats.limitExempt));
    }

    bob->append("exhaustIsMaster",
                static_cast<int>(HelloMetrics::get(_svcCtx)->getNumExhaustIsMaster()));
    bob->append("exhaustHello", static_cast<int>(HelloMetrics::get(_svcCtx)->getNumExhaustHello()));
    bob->append("awaitingTopologyChanges",
                static_cast<int>(HelloMetrics::get(_svcCtx)->getNumAwaitingTopologyChanges()));

    if (auto adminExec = transport::ServiceExecutorReserved::get(_svcCtx)) {
        BSONObjBuilder section(bob->subobjStart("adminConnections"));
        adminExec->appendStats(&section);
    }
}

}  // namespace mongo
