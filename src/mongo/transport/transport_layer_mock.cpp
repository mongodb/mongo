/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_mock.h"

#include "mongo/base/status.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

TransportLayerMock::TransportLayerMock() : _shutdown(false) {}

SessionHandle TransportLayerMock::createSession() {
    auto session = createSessionHook ? createSessionHook(this) : MockSession::create(this);
    Session::Id sessionId = session->id();

    _sessions[sessionId] = Connection{false, session, SSLPeerInfo()};

    return _sessions[sessionId].session;
}

SessionHandle TransportLayerMock::get(Session::Id id) {
    if (!owns(id))
        return nullptr;

    return _sessions[id].session;
}

bool TransportLayerMock::owns(Session::Id id) {
    return _sessions.count(id) > 0;
}

StatusWith<SessionHandle> TransportLayerMock::connect(HostAndPort peer,
                                                      ConnectSSLMode sslMode,
                                                      Milliseconds timeout) {
    MONGO_UNREACHABLE;
}

Future<SessionHandle> TransportLayerMock::asyncConnect(HostAndPort peer,
                                                       ConnectSSLMode sslMode,
                                                       const ReactorHandle& reactor,
                                                       Milliseconds timeout) {
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

}  // namespace transport
}  // namespace mongo
