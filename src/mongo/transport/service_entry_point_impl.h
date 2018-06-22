/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once


#include "mongo/base/disallow_copying.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/variant.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/service_state_machine.h"
#include "mongo/util/net/cidr.h"

namespace mongo {
class ServiceContext;

namespace transport {
class Session;
}  // namespace transport

/**
 * A basic entry point from the TransportLayer into a server.
 *
 * The server logic is implemented inside of handleRequest() by a subclass.
 * startSession() spawns and detaches a new thread for each incoming connection
 * (transport::Session).
 */
class ServiceEntryPointImpl : public ServiceEntryPoint {
    MONGO_DISALLOW_COPYING(ServiceEntryPointImpl);

public:
    explicit ServiceEntryPointImpl(ServiceContext* svcCtx);

    void startSession(transport::SessionHandle session) final;

    void endAllSessions(transport::Session::TagMask tags) final;

    Status start() final;
    bool shutdown(Milliseconds timeout) final;

    void appendStats(BSONObjBuilder* bob) const final;

    size_t numOpenSessions() const final {
        return _currentConnections.load();
    }

private:
    using SSMList = stdx::list<std::shared_ptr<ServiceStateMachine>>;
    using SSMListIterator = SSMList::iterator;

    ServiceContext* const _svcCtx;
    AtomicWord<std::size_t> _nWorkers;

    mutable stdx::mutex _sessionsMutex;
    stdx::condition_variable _shutdownCondition;
    SSMList _sessions;

    size_t _maxNumConnections{DEFAULT_MAX_CONN};
    AtomicWord<size_t> _currentConnections{0};
    AtomicWord<size_t> _createdConnections{0};

    std::unique_ptr<transport::ServiceExecutorReserved> _adminInternalPool;
};

/*
 * Returns true if a session with remote/local addresses should be exempted from maxConns
 */
bool shouldOverrideMaxConns(const transport::SessionHandle& session,
                            const std::vector<stdx::variant<CIDR, std::string>>& exemptions);

}  // namespace mongo
