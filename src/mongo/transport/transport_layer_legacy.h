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

#pragma once

#include <vector>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/sock.h"

namespace mongo {

class AbstractMessagingPort;
class ServiceContext;
class ServiceEntryPoint;
struct ServerGlobalParams;

namespace transport {

/**
 * A TransportLayer implementation based on legacy networking primitives (the Listener,
 * AbstractMessagingPort).
 */
class TransportLayerLegacy final : public TransportLayer {
    MONGO_DISALLOW_COPYING(TransportLayerLegacy);

public:
    struct Options {
        Options(const ServerGlobalParams* params);
        Options() : port(0), ipList() {}

        int port;                         // port to bind to
        std::vector<std::string> ipList;  // addresses to bind to
    };

    TransportLayerLegacy(const Options& opts, ServiceEntryPoint* sep);

    ~TransportLayerLegacy();

    Status setup() override;
    Status start() override;

    Ticket sourceMessage(const SessionHandle& session,
                         Message* message,
                         Date_t expiration = Ticket::kNoExpirationDate) override;

    Ticket sinkMessage(const SessionHandle& session,
                       const Message& message,
                       Date_t expiration = Ticket::kNoExpirationDate) override;

    Status wait(Ticket&& ticket) override;
    void asyncWait(Ticket&& ticket, TicketCallback callback) override;

    void end(const SessionHandle& session) override;

    void shutdown() override;

private:
    class LegacySession;
    using LegacySessionHandle = std::shared_ptr<LegacySession>;
    using ConstLegacySessionHandle = std::shared_ptr<const LegacySession>;

    void _destroy(LegacySession& session);

    void _handleNewConnection(std::unique_ptr<AbstractMessagingPort> amp);

    Status _runTicket(Ticket ticket);

    using NewConnectionCb = stdx::function<void(std::unique_ptr<AbstractMessagingPort>)>;
    using WorkHandle = stdx::function<Status(AbstractMessagingPort*)>;

    /**
     * Connection object, to associate Sessions with AbstractMessagingPorts.
     */
    struct Connection {
        MONGO_DISALLOW_COPYING(Connection);

        Connection(std::unique_ptr<AbstractMessagingPort> port)
            : amp(std::move(port)), connectionId(amp->connectionId()) {}

        stdx::mutex closeMutex;

        std::unique_ptr<AbstractMessagingPort> amp;

        const long long connectionId;

        boost::optional<SSLPeerInfo> sslPeerInfo;
        bool closed = false;
    };

    /**
     * An implementation of the Session interface for this TransportLayer.
     */
    class LegacySession : public Session {
        MONGO_DISALLOW_COPYING(LegacySession);

    public:
        ~LegacySession();

        static LegacySessionHandle create(std::unique_ptr<AbstractMessagingPort> amp,
                                          TransportLayerLegacy* tl);

        TransportLayer* getTransportLayer() const override {
            return _tl;
        }

        const HostAndPort& remote() const override {
            return _remote;
        }

        const HostAndPort& local() const override {
            return _local;
        }

        Connection* conn() const {
            return _connection.get();
        }

    private:
        explicit LegacySession(std::unique_ptr<AbstractMessagingPort> amp,
                               TransportLayerLegacy* tl);

        HostAndPort _remote;
        HostAndPort _local;

        TransportLayerLegacy* _tl;

        std::unique_ptr<Connection> _connection;
    };

    /**
     * A TicketImpl implementation for this TransportLayer. WorkHandle is a callable that
     * can be invoked to fill this ticket.
     */
    class LegacyTicket : public TicketImpl {
        MONGO_DISALLOW_COPYING(LegacyTicket);

    public:
        LegacyTicket(const LegacySessionHandle& session, Date_t expiration, WorkHandle work);

        SessionId sessionId() const override;
        Date_t expiration() const override;

        /**
         * If this ticket's session is still alive, return a shared_ptr. Otherwise,
         * return nullptr.
         */
        LegacySessionHandle getSession();

        /**
         * Run this ticket's work item.
         */
        Status fill(AbstractMessagingPort* amp);

    private:
        std::weak_ptr<LegacySession> _session;

        SessionId _sessionId;
        Date_t _expiration;

        WorkHandle _fill;
    };

    /**
     * This Listener wraps the interface in listen.h so that we may implement our own
     * version of accepted().
     */
    class ListenerLegacy : public Listener {
    public:
        ListenerLegacy(const TransportLayerLegacy::Options& opts, NewConnectionCb callback);

        void runListener();

        void accepted(std::unique_ptr<AbstractMessagingPort> mp) override;

        bool useUnixSockets() const override {
            return true;
        }

    private:
        NewConnectionCb _accepted;
    };

    void _closeConnection(Connection* conn);

    ServiceEntryPoint* _sep;

    std::unique_ptr<Listener> _listener;
    stdx::thread _listenerThread;

    AtomicWord<bool> _running;

    Options _options;
};

}  // namespace transport
}  // namespace mongo
