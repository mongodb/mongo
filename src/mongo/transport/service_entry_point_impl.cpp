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

#include <vector>

#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/ismaster_metrics.h"
#include "mongo/transport/service_state_machine.h"
#include "mongo/transport/session.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace mongo {

bool shouldOverrideMaxConns(const transport::SessionHandle& session,
                            const std::vector<stdx::variant<CIDR, std::string>>& exemptions) {
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

ServiceEntryPointImpl::ServiceEntryPointImpl(ServiceContext* svcCtx) : _svcCtx(svcCtx) {

    const auto supportedMax = [] {
#ifdef _WIN32
        return serverGlobalParams.maxConns;
#else
        struct rlimit limit;
        verify(getrlimit(RLIMIT_NOFILE, &limit) == 0);

        size_t max = (size_t)(limit.rlim_cur * .8);

        LOGV2_DEBUG(22940,
                    1,
                    "fd limit hard:{limit_rlim_max} soft:{limit_rlim_cur} max conn: {max}",
                    "limit_rlim_max"_attr = limit.rlim_max,
                    "limit_rlim_cur"_attr = limit.rlim_cur,
                    "max"_attr = max);

        return std::min(max, serverGlobalParams.maxConns);
#endif
    }();

    // If we asked for more connections than supported, inform the user.
    if (supportedMax < serverGlobalParams.maxConns &&
        serverGlobalParams.maxConns != DEFAULT_MAX_CONN) {
        LOGV2(22941,
              " --maxConns too high, can only handle {supportedMax}",
              "supportedMax"_attr = supportedMax);
    }

    _maxNumConnections = supportedMax;

    if (serverGlobalParams.reservedAdminThreads) {
        _adminInternalPool = std::make_unique<transport::ServiceExecutorReserved>(
            _svcCtx, "admin/internal connections", serverGlobalParams.reservedAdminThreads);
    }
}

Status ServiceEntryPointImpl::start() {
    if (_adminInternalPool)
        return _adminInternalPool->start();
    else
        return Status::OK();
}

void ServiceEntryPointImpl::startSession(transport::SessionHandle session) {
    // Setup the restriction environment on the Session, if the Session has local/remote Sockaddrs
    const auto& remoteAddr = session->remoteAddr();
    const auto& localAddr = session->localAddr();
    invariant(remoteAddr.isValid() && localAddr.isValid());
    auto restrictionEnvironment = std::make_unique<RestrictionEnvironment>(remoteAddr, localAddr);
    RestrictionEnvironment::set(session, std::move(restrictionEnvironment));

    SSMListIterator ssmIt;

    const bool quiet = serverGlobalParams.quiet.load();
    size_t connectionCount;
    auto transportMode = _svcCtx->getServiceExecutor()->transportMode();

    auto ssm = ServiceStateMachine::create(_svcCtx, session, transportMode);
    auto usingMaxConnOverride = false;
    {
        stdx::lock_guard<decltype(_sessionsMutex)> lk(_sessionsMutex);
        connectionCount = _sessions.size() + 1;
        if (connectionCount > _maxNumConnections) {
            usingMaxConnOverride =
                shouldOverrideMaxConns(session, serverGlobalParams.maxConnsOverride);
        }

        if (connectionCount <= _maxNumConnections || usingMaxConnOverride) {
            ssmIt = _sessions.emplace(_sessions.begin(), ssm);
            _currentConnections.store(connectionCount);
            _createdConnections.addAndFetch(1);
        }
    }

    // Checking if we successfully added a connection above. Separated from the lock so we don't log
    // while holding it.
    if (connectionCount > _maxNumConnections && !usingMaxConnOverride) {
        if (!quiet) {
            LOGV2(22942,
                  "connection refused because too many open connections",
                  "connectionCount"_attr = connectionCount);
        }
        return;
    } else if (usingMaxConnOverride && _adminInternalPool) {
        ssm->setServiceExecutor(_adminInternalPool.get());
    }

    if (!quiet) {
        LOGV2(22943,
              "connection accepted",
              "remote"_attr = session->remote(),
              "sessionId"_attr = session->id(),
              "connectionCount"_attr = connectionCount);
    }

    ssm->setCleanupHook([this, ssmIt, quiet, session = std::move(session)] {
        size_t connectionCount;
        auto remote = session->remote();
        {
            stdx::lock_guard<decltype(_sessionsMutex)> lk(_sessionsMutex);
            _sessions.erase(ssmIt);
            connectionCount = _sessions.size();
            _currentConnections.store(connectionCount);
        }
        _shutdownCondition.notify_one();

        if (!quiet) {
            LOGV2(22944,
                  "connection ended",
                  "remote"_attr = remote,
                  "connectionCount"_attr = connectionCount);
        }
    });

    auto ownership = ServiceStateMachine::Ownership::kOwned;
    if (transportMode == transport::Mode::kSynchronous) {
        ownership = ServiceStateMachine::Ownership::kStatic;
    }
    ssm->start(ownership);
}

void ServiceEntryPointImpl::endAllSessions(transport::Session::TagMask tags) {
    // While holding the _sesionsMutex, loop over all the current connections, and if their tags
    // do not match the requested tags to skip, terminate the session.
    {
        stdx::unique_lock<decltype(_sessionsMutex)> lk(_sessionsMutex);
        for (auto& ssm : _sessions) {
            ssm->terminateIfTagsDontMatch(tags);
        }
    }
}

bool ServiceEntryPointImpl::shutdown(Milliseconds timeout) {
    using logv2::LogComponent;

    stdx::unique_lock<decltype(_sessionsMutex)> lk(_sessionsMutex);

    // Request that all sessions end, while holding the _sesionsMutex, loop over all the current
    // connections and terminate them
    for (auto& ssm : _sessions) {
        ssm->terminate();
    }

    // Close all sockets and then wait for the number of active connections to reach zero with a
    // condition_variable that notifies in the session cleanup hook. If we haven't closed drained
    // all active operations within the deadline, just keep going with shutdown: the OS will do it
    // for us when the process terminates.
    auto timeSpent = Milliseconds(0);
    const auto checkInterval = std::min(Milliseconds(250), timeout);

    auto noWorkersLeft = [this] { return numOpenSessions() == 0; };
    while (timeSpent < timeout &&
           !_shutdownCondition.wait_for(lk, checkInterval.toSystemDuration(), noWorkersLeft)) {
        LOGV2(22945,
              "shutdown: still waiting on {numOpenSessions} active workers to drain... ",
              "numOpenSessions"_attr = numOpenSessions());
        timeSpent += checkInterval;
    }

    bool result = noWorkersLeft();
    if (result) {
        LOGV2(22946, "shutdown: no running workers found...");
    } else {
        LOGV2(22947,
              "shutdown: exhausted grace period for{numOpenSessions} active workers to "
              "drain; continuing with shutdown... ",
              "numOpenSessions"_attr = numOpenSessions());
    }
    return result;
}

void ServiceEntryPointImpl::appendStats(BSONObjBuilder* bob) const {

    size_t sessionCount = _currentConnections.load();

    bob->append("current", static_cast<int>(sessionCount));
    bob->append("available", static_cast<int>(_maxNumConnections - sessionCount));
    bob->append("totalCreated", static_cast<int>(_createdConnections.load()));
    if (auto sc = getGlobalServiceContext()) {
        bob->append("active", static_cast<int>(sc->getActiveClientOperations()));
        bob->append("exhaustIsMaster",
                    static_cast<int>(IsMasterMetrics::get(sc)->getNumExhaustIsMaster()));
        bob->append("awaitingTopologyChanges",
                    static_cast<int>(IsMasterMetrics::get(sc)->getNumAwaitingTopologyChanges()));
    }

    if (_adminInternalPool) {
        BSONObjBuilder section(bob->subobjStart("adminConnections"));
        _adminInternalPool->appendStats(&section);
    }
}

}  // namespace mongo
