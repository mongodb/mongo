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
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_mode.h"
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
        std::vector<std::string> ipList;               // addresses to bind to
#ifndef _WIN32
        bool useUnixSockets = true;  // whether to allow UNIX sockets in ipList
#endif
        bool enableIPv6 = false;                  // whether to allow IPv6 sockets in ipList
        Mode transportMode = Mode::kSynchronous;  // whether accepted sockets should be put into
                                                  // non-blocking mode after they're accepted
        size_t maxConns = DEFAULT_MAX_CONN;       // maximum number of active connections
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

    void end(const SessionHandle& session) final;

    Status setup() final;
    Status start() final;

    void shutdown() final;

    const std::shared_ptr<asio::io_context>& getIOContext();

private:
    class ASIOSession;
    class ASIOTicket;
    class ASIOSourceTicket;
    class ASIOSinkTicket;

    using ASIOSessionHandle = std::shared_ptr<ASIOSession>;
    using ConstASIOSessionHandle = std::shared_ptr<const ASIOSession>;
    using GenericAcceptor = asio::basic_socket_acceptor<asio::generic::stream_protocol>;

    void _acceptConnection(GenericAcceptor& acceptor);
#ifdef MONGO_CONFIG_SSL
    SSLParams::SSLModes _sslMode() const;
#endif

    stdx::mutex _mutex;

    // There are two IO contexts that are used by TransportLayerASIO. The _workerIOContext
    // contains all the accepted sockets and all normal networking activity. The
    // _acceptorIOContext contains all the sockets in _acceptors.
    //
    // TransportLayerASIO should never call run() on the _workerIOContext.
    // In synchronous mode, this will cause a massive performance degradation due to
    // unnecessary wakeups on the asio thread for sockets we don't intend to interact
    // with asynchronously. The additional IO context avoids registering those sockets
    // with the acceptors epoll set, thus avoiding those wakeups.  Calling run will
    // undo that benefit.
    //
    // TransportLayerASIO should run its own thread that calls run() on the _acceptorIOContext
    // to process calls to async_accept - this is the equivalent of the "listener" thread in
    // other TransportLayers.
    //
    // The underlying problem that caused this is here:
    // https://github.com/chriskohlhoff/asio/issues/240
    //
    // It is important that the io_context be declared before the
    // vector of acceptors (or any other state that is associated with
    // the io_context), so that we destroy any existing acceptors or
    // other io_service associated state before we drop the refcount
    // on the io_context, which may destroy it.
    std::shared_ptr<asio::io_context> _workerIOContext;
    std::unique_ptr<asio::io_context> _acceptorIOContext;

#ifdef MONGO_CONFIG_SSL
    std::unique_ptr<asio::ssl::context> _sslContext;
#endif

    std::vector<std::pair<SockAddr, GenericAcceptor>> _acceptors;

    // Only used if _listenerOptions.async is false.
    stdx::thread _listenerThread;

    ServiceEntryPoint* const _sep = nullptr;
    AtomicWord<bool> _running{false};
    Options _listenerOptions;
};

}  // namespace transport
}  // namespace mongo
