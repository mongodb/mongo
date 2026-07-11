// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/session.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <boost/optional.hpp>

namespace mongo {
namespace transport {
class SessionManager;
}  // namespace transport

class HelloMetrics;

/**
 * A decoration on the Session object used to track exhaust metrics. We are
 * tracking metrics for "hello" and "isMaster/ismaster" separately while we
 * support both commands. This allows us insight into which command is being
 * used until we decide to remove support for isMaster completely.
 */
class [[MONGO_MOD_PUBLIC]] InExhaustHello {
public:
    enum class Command {
        kHello,
        kIsMaster,
    };

    InExhaustHello() = default;

    InExhaustHello(const InExhaustHello&) = delete;
    InExhaustHello& operator=(const InExhaustHello&) = delete;
    InExhaustHello(InExhaustHello&&) = delete;
    InExhaustHello& operator=(InExhaustHello&&) = delete;

    static InExhaustHello* get(transport::Session* session);
    void setInExhaust(Command command);
    void resetInExhaust();
    bool getInExhaustIsMaster() const;
    bool getInExhaustHello() const;
    ~InExhaustHello();

private:
    void transitionOutOfInExhaustHello(HelloMetrics*);
    void transitionOutOfInExhaustIsMaster(HelloMetrics*);

    bool _inExhaustIsMaster = false;
    bool _inExhaustHello = false;

    // In most cases, SessionManager can be derived from
    // Decoration.owner(decoration)->getTransportLayer()->getSessionManager().
    // However, during the destructor call, when we need to restore HelloMetrics counts,
    // the Session pointer is no longer valid and getTransportLayer() is invalid.
    // Stash a weak pointer to the SessionManager associated with our HelloMetrics
    // during initial load of InExhaustHello.
    boost::optional<std::weak_ptr<transport::SessionManager>> _sessionManager;
};

/**
 * Container for awaitable hello and isMaster statistics. We are tracking
 * metrics for "hello" and "isMaster/ismaster" separately while we support
 * both commands. This allows us insight into which command is being used
 * until we decide to remove support for isMaster completely.
 */
class [[MONGO_MOD_PUBLIC]] HelloMetrics {
    HelloMetrics(const HelloMetrics&) = delete;
    HelloMetrics& operator=(const HelloMetrics&) = delete;
    HelloMetrics(HelloMetrics&&) = delete;
    HelloMetrics& operator=(HelloMetrics&&) = delete;

public:
    HelloMetrics() = default;

    HelloMetrics& operator+=(const HelloMetrics& other);

    // Convenience accessor for acquiring HelloMetrics from a SessionManager.
    static HelloMetrics* get(OperationContext* opCtx);

    size_t getNumExhaustIsMaster() const;
    size_t getNumExhaustHello() const;

    size_t getNumAwaitingTopologyChanges() const;
    void incrementNumAwaitingTopologyChanges();
    void decrementNumAwaitingTopologyChanges();

    friend InExhaustHello;

    /**
     * Loops through all SessionManagers on the ServiceContext's TransportLayerManager
     * and resets their numAwaitingTopologyChanges.
     */
    static void resetNumAwaitingTopologyChangesForAllSessionManagers(ServiceContext*);

    void serialize(BSONObjBuilder*) const;

private:
    void resetNumAwaitingTopologyChanges();
    void incrementNumExhaustIsMaster();
    void decrementNumExhaustIsMaster();

    void incrementNumExhaustHello();
    void decrementNumExhaustHello();

    // The number of clients currently waiting in isMaster for a topology change.
    Atomic<size_t> _connectionsAwaitingTopologyChanges{0};
    // The number of connections whose last request was an isMaster with exhaustAllowed.
    Atomic<size_t> _exhaustIsMasterConnections{0};
    // The number of connections whose last request was a hello with exhaustAllowed.
    Atomic<size_t> _exhaustHelloConnections{0};
};

}  // namespace mongo
