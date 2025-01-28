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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <functional>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_metrics.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#endif

namespace mongo {

class OperationContext;

namespace transport {

enum ConnectSSLMode { kGlobalSSLMode, kEnableSSL, kDisableSSL };
enum class TransportProtocol { MongoRPC, GRPC };

class Reactor;
using ReactorHandle = std::shared_ptr<Reactor>;
class SessionManager;

/**
 * The TransportLayer moves Messages between transport::Endpoints and the database.
 * This class owns an Acceptor that generates new endpoints from which it can
 * source Messages.
 *
 * The TransportLayer creates Session objects and maps them internally to
 * endpoints. New Sessions are passed to the database (via a ServiceEntryPoint)
 * to be run. The database must then call additional methods on the TransportLayer
 * to manage the Session in a get-Message, handle-Message, return-Message cycle.
 * It must do this on its own thread(s).
 *
 * References to the TransportLayer should be stored on service context objects.
 */
class TransportLayer {
    TransportLayer(const TransportLayer&) = delete;
    TransportLayer& operator=(const TransportLayer&) = delete;

public:
    static const Status SessionUnknownStatus;
    static const Status ShutdownStatus;
    static const Status TicketSessionUnknownStatus;
    static const Status TicketSessionClosedStatus;

    friend class Session;

    TransportLayer() = default;
    virtual ~TransportLayer() = default;

    virtual StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams = boost::none) = 0;

    virtual Future<std::shared_ptr<Session>> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext) = 0;

    /**
     * Start the TransportLayer. After this point, the TransportLayer will begin accepting active
     * sessions from new transport::Endpoints.
     */
    virtual Status start() = 0;

    /**
     * Shut the TransportLayer down. After this point, the TransportLayer will
     * end all active sessions and won't accept new transport::Endpoints. Any
     * future calls to wait() or asyncWait() will fail. This method is synchronous and
     * will not return until all sessions have ended and any network connections have been
     * closed.
     */
    virtual void shutdown() = 0;

    /**
     * Stop accepting new sessions.
     */
    virtual void stopAcceptingSessions() = 0;

    /**
     * Optional method for subclasses to setup their state before being ready to accept
     * connections.
     */
    virtual Status setup() = 0;

    /** Allows a `TransportLayer` to contribute to a serverStatus readout. */
    virtual void appendStatsForServerStatus(BSONObjBuilder* bob) const {}

    /** Allows a `TransportLayer` to contribute to a FTDC readout. */
    virtual void appendStatsForFTDC(BSONObjBuilder& bob) const {}

    virtual StringData getNameForLogging() const = 0;

    enum WhichReactor { kIngress, kEgress, kNewReactor };
    virtual ReactorHandle getReactor(WhichReactor which) = 0;

    virtual BatonHandle makeBaton(OperationContext* opCtx) const {
        return {};
    }

    /**
     * Returns the TransportProtocol associated with this TransportLayer.
     */
    virtual TransportProtocol getTransportProtocol() const = 0;

    /**
     * Return the session manager, if any, associated with this TransportLayer.
     */
    virtual SessionManager* getSessionManager() const = 0;

    /**
     * Returns whether or not the TransporLayer is configured to use ingress networking.
     */
    virtual bool isIngress() const = 0;

    /**
     * Returns whether or not the TransporLayer is configured to use egress networking.
     */
    virtual bool isEgress() const = 0;

    /**
     * Returns a shared_ptr reference to the owned SessionManager.
     * Callers are strongly discouraged from retaining a full shared_ptr
     * reference which may cause the SessionManager to outlive its TransportLayer.
     * Please convert to `std::weak_ptr` if a long term, non-owning reference is needed.
     */
    virtual std::shared_ptr<SessionManager> getSharedSessionManager() const = 0;

#ifdef MONGO_CONFIG_SSL
    /** Rotate the in-use certificates for new connections. */
    virtual Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                      bool asyncOCSPStaple) = 0;

    /**
     * Creates a transient SSL context using targeted (non default) SSL params.
     * @param transientSSLParams overrides any value in stored SSLConnectionContext.
     * @param optionalManager provides an optional SSL manager, otherwise the default one will be
     * used.
     */
    virtual StatusWith<std::shared_ptr<const transport::SSLConnectionContext>>
    createTransientSSLContext(const TransientSSLParams& transientSSLParams) = 0;
#endif
};

class ReactorTimer {
public:
    ReactorTimer();

    ReactorTimer(const ReactorTimer&) = delete;
    ReactorTimer& operator=(const ReactorTimer&) = delete;

    /*
     * The destructor calls cancel() to ensure the outstanding Future is filled.
     */
    virtual ~ReactorTimer() = default;

    size_t id() const {
        return _id;
    }

    /*
     * Cancel any outstanding future from waitFor/waitUntil. The future will be filled with an
     * ErrorCodes::CallbackCancelled status.
     *
     * If no future is outstanding, then this is a noop.
     */
    virtual void cancel(const BatonHandle& baton = nullptr) = 0;

    /*
     * Returns a future that will be filled with Status::OK after the deadline has passed.
     *
     * Calling this implicitly calls cancel(), as there can be at most one outstanding Future per
     * ReactorTimer at a time.
     */
    virtual Future<void> waitUntil(Date_t deadline, const BatonHandle& baton = nullptr) = 0;

private:
    const size_t _id;
};

/**
 * The Reactor is an OutOfLineExecutor that uses the event engine of the underlying transport layer
 * to schedule and execute work. Work is scheduled on the reactor through calls to
 * schedule and through performing async networking work on the relevant event engine primitive for
 * each transport layer. The reactor is typically used by starting a separate thread that calls
 * run() and drain().
 *
 * All Session objects associated with a reactor MUST be ended before the reactor is stopped.
 */
class Reactor : public OutOfLineExecutor {
public:
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    ~Reactor() override = default;

    /*
     * Run the event loop of the reactor until stop() is called.
     */
    virtual void run() = 0;

    /**
     * Stop the polling loop in run(). drain() must be called after stop() to ensure no outstanding
     * tasks are leaked.
     */
    virtual void stop() = 0;

    /**
     * This function will block until all outstanding work scheduled on the reactor has been
     * completed or canceled.
     */
    virtual void drain() = 0;

    /**
     * Schedule a task to run on the reactor thread. See OutOfLineExecutor::schedule for more
     * detailed documentation.
     */
    void schedule(Task task) override = 0;

    bool onReactorThread() const {
        return this == _reactorForThread;
    }

    /*
     * Makes a timer tied to this reactor's event loop. Timeout callbacks will be
     * executed in a thread calling run().
     */
    virtual std::unique_ptr<ReactorTimer> makeTimer() = 0;

    /**
     * Get the time according to the clock driving the event engine of the reactor.
     */
    virtual Date_t now() = 0;

    /**
     * Appends stats for the reactor, typically recorded with the ExecutorStats class.
     */
    virtual void appendStats(BSONObjBuilder& bob) const = 0;

protected:
    Reactor() = default;

    /**
     * Helper class for the onReactorThread function. Implementations of the Reactor should use the
     * ThreadIdGuard in the run() and drain() functions.
     */
    class ThreadIdGuard {
    public:
        ThreadIdGuard(Reactor* reactor) {
            invariant(!_reactorForThread);
            _reactorForThread = reactor;
        }

        ~ThreadIdGuard() {
            invariant(_reactorForThread);
            _reactorForThread = nullptr;
        }
    };

    /**
     * Provides `ClockSource` API for the reactor's clock source, which can be used to record
     * ExecutorStats.
     */
    class ReactorClockSource final : public ClockSource {
    public:
        explicit ReactorClockSource(Reactor* reactor) : _reactor(reactor) {}
        ~ReactorClockSource() override = default;

        Milliseconds getPrecision() override {
            MONGO_UNREACHABLE;
        }

        Date_t now() override;

    private:
        Reactor* const _reactor;
    };

    static thread_local Reactor* _reactorForThread;
};


}  // namespace transport
}  // namespace mongo
