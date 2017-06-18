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

#include <functional>
#include <string>

#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"

namespace asio {
class io_context;

template <typename Protocol>
class basic_socket_acceptor;

namespace generic {
class stream_protocol;
}  // namespace generic

namespace ssl {
class context;
}  // namespace ssl
}  // namespace asio

namespace mongo {

class ServiceContext;
class ServiceEntryPoint;

namespace transport {

/**
 * A TransportLayer implementation based on ASIO networking primitives.
 */
class TransportLayerASIO final : public TransportLayer {
    MONGO_DISALLOW_COPYING(TransportLayerASIO);

public:
    struct Options {
        explicit Options(const ServerGlobalParams* params);

        int port = ServerGlobalParams::DefaultDBPort;  // port to bind to
        std::string ipList;                            // addresses to bind to
#ifndef _WIN32
        bool useUnixSockets = true;  // whether to allow UNIX sockets in ipList
#endif
        bool enableIPv6 = false;             // whether to allow IPv6 sockets in ipList
        bool async = false;                  // whether accepted sockets should be put into
                                             // non-blocking mode after they're accepted
        size_t maxConns = DEFAULT_MAX_CONN;  // maximum number of active connections
    };

    TransportLayerASIO(const Options& opts, ServiceEntryPoint* sep);

    virtual ~TransportLayerASIO();

    Ticket sourceMessage(const SessionHandle& session,
                         Message* message,
                         Date_t expiration = Ticket::kNoExpirationDate) final;

    Ticket sinkMessage(const SessionHandle& session,
                       const Message& message,
                       Date_t expiration = Ticket::kNoExpirationDate) final;

    Status wait(Ticket&& ticket) final;

    void asyncWait(Ticket&& ticket, TicketCallback callback) final;

    Stats sessionStats() final;

    void end(const SessionHandle& session) final;

    void endAllSessions(transport::Session::TagMask tags) final;

    Status setup() final;
    Status start() final;

    void shutdown() final;

private:
    class ASIOSession;
    class ASIOTicket;
    class ASIOSourceTicket;
    class ASIOSinkTicket;
    class SessionListGuard;

    using ASIOSessionHandle = std::shared_ptr<ASIOSession>;
    using ConstASIOSessionHandle = std::shared_ptr<const ASIOSession>;
    using GenericAcceptor = asio::basic_socket_acceptor<asio::generic::stream_protocol>;
    using SessionsListIterator = stdx::list<std::weak_ptr<ASIOSession>>::iterator;

    ASIOSessionHandle createSession();
    void eraseSession(SessionsListIterator it);
    void _acceptConnection(GenericAcceptor& acceptor);

    stdx::mutex _mutex;
    std::vector<GenericAcceptor> _acceptors;
    stdx::list<std::weak_ptr<ASIOSession>> _sessions;

    // Only used if _listenerOptions.async is false.
    stdx::thread _listenerThread;

    std::unique_ptr<asio::io_context> _ioContext;
#ifdef MONGO_CONFIG_SSL
    std::unique_ptr<asio::ssl::context> _sslContext;
    SSLParams::SSLModes _sslMode;
#endif

    ServiceEntryPoint* const _sep = nullptr;
    AtomicWord<bool> _running{false};
    Options _listenerOptions;

    AtomicWord<size_t> _createdConnections{0};
};

}  // namespace transport
}  // namespace mongo
