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

#include "mongo/transport/transport_layer_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/transport/mock_session.h"
#include "mongo/transport/session_manager_noop.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace transport {

TransportLayerMock::TransportLayerMock()
    : TransportLayerMock(std::make_unique<SessionManagerNoop>()) {}

std::shared_ptr<Session> TransportLayerMock::createSession() {
    auto session = createSessionHook ? createSessionHook(this) : MockSession::create(this);
    Session::Id sessionId = session->id();

    _sessions[sessionId] = Connection{false, session, SSLPeerInfo()};

    return _sessions[sessionId].session;
}

std::shared_ptr<Session> TransportLayerMock::get(Session::Id id) {
    if (!owns(id))
        return nullptr;

    return _sessions[id].session;
}

bool TransportLayerMock::owns(Session::Id id) {
    return _sessions.count(id) > 0;
}

void TransportLayerMock::deleteSession(Session::Id id) {
    _sessions.erase(id);
}

StatusWith<std::shared_ptr<Session>> TransportLayerMock::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    const boost::optional<TransientSSLParams>& transientSSLParams) {
    MONGO_UNREACHABLE;
}

Future<std::shared_ptr<Session>> TransportLayerMock::asyncConnect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    std::shared_ptr<const SSLConnectionContext> transientSSLContext) {
    MONGO_UNREACHABLE;
}

Status TransportLayerMock::setup() {
    return Status::OK();
}

Status TransportLayerMock::start() {
    return Status::OK();
}

void TransportLayerMock::shutdown() {
    if (!inShutdown()) {
        _shutdown = true;
    }
}

ReactorHandle TransportLayerMock::getReactor(WhichReactor which) {
    return nullptr;
}

bool TransportLayerMock::inShutdown() const {
    return _shutdown;
}

TransportLayerMock::~TransportLayerMock() {
    shutdown();
}

#ifdef MONGO_CONFIG_SSL

StatusWith<std::shared_ptr<const transport::SSLConnectionContext>>
TransportLayerMock::createTransientSSLContext(const TransientSSLParams& transientSSLParams) {
    return Status(ErrorCodes::InvalidSSLConfiguration, "Failure creating transient SSL context");
}

#endif

}  // namespace transport
}  // namespace mongo
