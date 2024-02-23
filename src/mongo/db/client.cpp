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

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include <boost/container_hash/extensions.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_cpu_timer.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log_service.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

thread_local ServiceContext::UniqueClient currentClient;

void invariantNoCurrentClient() {
    invariant(!haveClient(),
              str::stream() << "Already have client on this thread: "  //
                            << '"' << Client::getCurrent()->desc() << '"');
}

int64_t generateSeed(const std::string& desc) {
    size_t seed = 0;
    boost::hash_combine(seed, Date_t::now().asInt64());
    boost::hash_combine(seed, desc);
    return seed;
}

bool checkIfRouterClient(const std::shared_ptr<transport::Session>& session) {
    return session && session->isFromRouterPort();
}

}  // namespace

void Client::initThread(StringData desc,
                        Service* service,
                        std::shared_ptr<transport::Session> session) {
    invariantNoCurrentClient();

    std::string fullDesc;
    if (session) {
        fullDesc = str::stream() << desc << session->id();
    } else {
        fullDesc = desc.toString();
    }

    setThreadName(fullDesc);

    // Create the client obj, attach to thread
    currentClient = service->makeClient(fullDesc, std::move(session));
    setLogService(toLogService(service));
}

void Client::setCurrent(ServiceContext::UniqueClient client) {
    invariantNoCurrentClient();
    setLogService(toLogService(client.get()->getService()));
    currentClient = std::move(client);
    if (auto opCtx = currentClient->_opCtx)
        if (auto timers = OperationCPUTimers::get(opCtx))
            timers->onThreadAttach();
}

ServiceContext::UniqueClient Client::releaseCurrent() {
    invariant(haveClient(), "No client to release");
    if (auto opCtx = currentClient->_opCtx)
        if (auto timers = OperationCPUTimers::get(opCtx))
            timers->onThreadDetach();
    setLogService(logv2::LogService::unknown);
    return std::move(currentClient);
}


Client* Client::getCurrent() {
    return currentClient.get();
}

Client::Client(std::string desc, Service* service, std::shared_ptr<transport::Session> session)
    : _service(service),
      _session(std::move(session)),
      _desc(std::move(desc)),
      _connectionId(_session ? _session->id() : 0),
      _prng(generateSeed(_desc)),
      _isRouterClient(checkIfRouterClient(_session)),
      _uuid(UUID::gen()),
      _tags(kPending) {}

Client::~Client() = default;

void Client::reportState(BSONObjBuilder& builder) {
    builder.append("desc", desc());

    if (_connectionId) {
        builder.appendNumber("connectionId", _connectionId);
    }

    if (hasRemote()) {
        builder.append("client", getRemote().toString());
    }
}

ServiceContext::UniqueOperationContext Client::makeOperationContext() {
    return getServiceContext()->makeOperationContext(this);
}

std::string Client::clientAddress(bool includePort) const {
    if (!hasRemote()) {
        return "";
    }
    if (includePort) {
        return getRemote().toString();
    }
    return getRemote().host();
}

Client& cc() {
    invariant(haveClient());
    return *Client::getCurrent();
}

bool haveClient() {
    return static_cast<bool>(currentClient);
}

/**
 * User connections are listed active so long as they are associated with an opCtx.
 * Non-user connections are listed active if they have an opCtx and not waiting on a condvar.
 */
bool Client::hasAnyActiveCurrentOp() const {
    if (!_opCtx)
        return false;
    if (isFromUserConnection() || !_opCtx->isWaitingForConditionOrInterrupt())
        return true;
    return false;
}

void Client::setKilled() noexcept {
    stdx::lock_guard<Client> lk(*this);
    _killed.store(true);
    if (_opCtx) {
        getServiceContext()->killOperation(lk, _opCtx, ErrorCodes::ClientMarkedKilled);
    }
}

ThreadClient::ThreadClient(StringData desc,
                           Service* service,
                           std::shared_ptr<transport::Session> session) {
    invariantNoCurrentClient();
    _originalThreadName = getThreadNameRef();
    Client::initThread(desc, service, std::move(session));
}

ThreadClient::~ThreadClient() {
    invariant(currentClient);
    currentClient.reset(nullptr);
    setLogService(logv2::LogService::unknown);
    setThreadNameRef(std::move(_originalThreadName));
}

Client* ThreadClient::get() const {
    return &cc();
}

AlternativeClientRegion::AlternativeClientRegion(ServiceContext::UniqueClient& clientToUse)
    : _alternateClient(&clientToUse) {
    invariant(clientToUse);
    if (Client::getCurrent()) {
        _originalClient = Client::releaseCurrent();
    }
    Client::setCurrent(std::move(*_alternateClient));
}

AlternativeClientRegion::~AlternativeClientRegion() {
    *_alternateClient = Client::releaseCurrent();
    if (_originalClient) {
        Client::setCurrent(std::move(_originalClient));
    }
}

Client* AlternativeClientRegion::get() const {
    return &cc();
}

void Client::setTags(TagMask tagsToSet) {
    mutateTags([tagsToSet](TagMask originalTags) { return (originalTags | tagsToSet); });
}

void Client::unsetTags(TagMask tagsToUnset) {
    mutateTags([tagsToUnset](TagMask originalTags) { return (originalTags & ~tagsToUnset); });
}

void Client::mutateTags(const std::function<TagMask(TagMask)>& mutateFunc) {
    TagMask oldValue, newValue;
    do {
        oldValue = _tags.load();
        newValue = mutateFunc(oldValue);

        // Any change to the Client tags automatically clears kPending status.
        newValue &= ~kPending;
    } while (!_tags.compareAndSwap(&oldValue, newValue));
}

Client::TagMask Client::getTags() const {
    return _tags.load();
}

int Client::getLocalPort() const {
    if (_service->role().hasExclusively(ClusterRole::RouterServer) &&
        serverGlobalParams.routerPort) {
        if (_opCtx && _opCtx->routedByReplicaSetEndpoint()) {
            // This is a client connected to the replica set endpoint so return the shard/main port.
            return serverGlobalParams.port;
        }
        return serverGlobalParams.routerPort.value();
    }
    return serverGlobalParams.port;
}

void Client::_setOperationContext(OperationContext* opCtx) {
    _opCtx = opCtx;
    if (_session) {
        _session->setInOperation(opCtx != nullptr);
    }
}

}  // namespace mongo
