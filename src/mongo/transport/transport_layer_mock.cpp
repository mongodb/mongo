// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
