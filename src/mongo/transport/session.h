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

#pragma once

#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/baton.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/session_id.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/time_support.h"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_types.h"
#endif

namespace mongo {

class SSLManagerInterface;

namespace transport {

class Session;
class SessionManager;
class TransportLayer;

/**
 * This type contains data needed to associate Messages with connections
 * (on the transport side) and Messages with Client objects (on the database side).
 */
class Session : public std::enable_shared_from_this<Session>, public Decorable<Session> {
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

public:
    /**
     * Type to indicate the internal id for this session.
     */
    using Id = SessionId;

    static const Status ClosedStatus;

    ~Session() override;

    Id id() const {
        return _id;
    }

    virtual TransportLayer* getTransportLayer() const = 0;

    /**
     * Signals the Session that an OperationContext is/is-not active on the associated Client.
     * This method is not thread safe and should only be called from the Client's thread.
     *
     * While this method is marked virtual, it is not actually meant to be overridden by
     * child classes.  The use of virtual here is to resolve an otherwise circular linking
     * by way of using the type erased vtable lookup at runtime.
     */
    virtual void setInOperation(bool state);

    /**
     * Ends this Session.
     *
     * Operations on this Session that have already been started via wait() or asyncWait() will
     * complete, but may return a failed Status.  Future operations on this Session will fail. If
     * this TransportLayer implementation is networked, any connections for this Session will be
     * closed.
     *
     * This method is idempotent and synchronous.
     *
     * Destructors of derived classes will close the session automatically if needed. This method
     * should only be called explicitly if the session should be closed separately from destruction,
     * eg due to some outside event.
     */
    virtual void end() = 0;

    /**
     * Source (receive) a new Message from the remote host for this Session.
     */
    virtual StatusWith<Message> sourceMessage() noexcept = 0;
    virtual Future<Message> asyncSourceMessage(const BatonHandle& handle = nullptr) noexcept = 0;

    /**
     * Waits for the availability of incoming data.
     */
    virtual Status waitForData() noexcept = 0;
    virtual Future<void> asyncWaitForData() noexcept = 0;

    /**
     * Sink (send) a Message to the remote host for this Session.
     *
     * Async version will keep the buffer alive until the operation completes.
     */
    virtual Status sinkMessage(Message message) noexcept = 0;
    virtual Future<void> asyncSinkMessage(Message message,
                                          const BatonHandle& handle = nullptr) noexcept = 0;

    /**
     * Cancel any outstanding async operations. There is no way to cancel synchronous calls.
     * Futures will finish with an ErrorCodes::CallbackCancelled error if they haven't already
     * completed.
     */
    virtual void cancelAsyncOperations(const BatonHandle& handle = nullptr) = 0;

    /**
     * This should only be used to detect when the remote host has disappeared without
     * notice. It does NOT work correctly for ensuring that operations complete or fail
     * by some deadline.
     *
     * This timeout will only effect calls sourceMessage()/sinkMessage(). Async operations do not
     * currently support timeouts.
     */
    virtual void setTimeout(boost::optional<Milliseconds> timeout) = 0;

    /**
     * This will return whether calling sourceMessage()/sinkMessage() will fail with an EOF error.
     *
     * Implementations may actually perform some I/O or call syscalls to determine this, rather
     * than just checking a flag.
     *
     * This must not be called while the session is currently sourcing or sinking a message.
     */
    virtual bool isConnected() = 0;

    /**
     * Returns true if this session was connected through an L4 load balancer.
     */
    virtual bool isFromLoadBalancer() const = 0;

    /**
     * Returns true if this session binds to the operation state, which implies open cursors and
     * in-progress transactions should be killed upon client disconnection.
     */
    virtual bool bindsToOperationState() const = 0;

    /**
     * Returns true if this session corresponds to a connection accepted from the router port.
     */
    virtual bool isFromRouterPort() const {
        return false;
    }

    /**
     * Returns the HostAndPort of the directly-connected remote
     * to this session.
     */
    virtual const HostAndPort& remote() const = 0;

    /**
     * Returns the HostAndPort of the local endpoint of this session.
     */
    virtual const HostAndPort& local() const = 0;

    /**
     * Returns the source remote endpoint. The server determines the
     * source remote endpoint via the following set of rules:
     *  1. If the connection was accepted via the load balancer port:
     *      a. If the TCP packet presented a valid proxy protocol header, then
     *         the source remote endpoint is a HostAndPort constructed from the
     *         source IP address and source port presented in that header.
     *      b. If the TCP packet did not present a valid proxy protocol header,
     *         then the transport layer will fail to parse and fail session establishment.
     *  2. If the connection was NOT accepted via the load balancer port:
     *      a. The source remote endpoint is always remote(). The proxy protocol
     *         header is only parsed if presented by a load balancer connection.
     */
    virtual const HostAndPort& getSourceRemoteEndpoint() const {
        return remote();
    }

    /**
     * Returns the proxy endpoint. The server determines the
     * proxy endpoint via the following set of rules:
     *  1. If the connection was accepted via the load balancer port:
     *      a. If the TCP packet presented a valid proxy protocol header, then
     *         the proxy endpoint is a HostAndPort constructed from the
     *         destination IP address and destination port presented in that header.
     *      b. If the TCP packet did not present a valid proxy protocol header,
     *         then the transport layer will fail to parse and fail session establishment.
     *  2. If the connection was NOT accepted via the load balancer port:
     *      a. The proxy endpoint is always boost::none since there are no proxies.
     */
    virtual boost::optional<const HostAndPort&> getProxiedDstEndpoint() const {
        return boost::none;
    }

    virtual void appendToBSON(BSONObjBuilder& bb) const = 0;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        appendToBSON(builder);
        return builder.obj();
    }

    virtual bool shouldOverrideMaxConns(
        const std::vector<std::variant<CIDR, std::string>>& exemptions) const = 0;

#ifdef MONGO_CONFIG_SSL
    /**
     * Get the SSL configuration associated with this session, if any.
     */
    virtual const SSLConfiguration* getSSLConfiguration() const = 0;
#endif

    virtual const RestrictionEnvironment& getAuthEnvironment() const = 0;

protected:
    Session();

private:
    const Id _id;
    bool _inOperation{false};
    std::weak_ptr<SessionManager> _sessionManager;
};

}  // namespace transport
}  // namespace mongo
