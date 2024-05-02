/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <cstddef>

#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session.h"

namespace mongo {
namespace transport {
class SessionManager;
}  // namespace transport

/**
 * A decoration on the Session object used to track exhaust metrics. We are
 * tracking metrics for "hello" and "isMaster/ismaster" separately while we
 * support both commands. This allows us insight into which command is being
 * used until we decide to remove support for isMaster completely.
 */
class InExhaustHello {
public:
    InExhaustHello() = default;

    InExhaustHello(const InExhaustHello&) = delete;
    InExhaustHello& operator=(const InExhaustHello&) = delete;
    InExhaustHello(InExhaustHello&&) = delete;
    InExhaustHello& operator=(InExhaustHello&&) = delete;

    static InExhaustHello* get(transport::Session* session);
    void setInExhaust(bool inExhaust, StringData commandName);
    bool getInExhaustIsMaster() const;
    bool getInExhaustHello() const;
    ~InExhaustHello();

private:
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
class HelloMetrics {
    HelloMetrics(const HelloMetrics&) = delete;
    HelloMetrics& operator=(const HelloMetrics&) = delete;
    HelloMetrics(HelloMetrics&&) = delete;
    HelloMetrics& operator=(HelloMetrics&&) = delete;

public:
    HelloMetrics() = default;

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
    AtomicWord<size_t> _connectionsAwaitingTopologyChanges{0};
    // The number of connections whose last request was an isMaster with exhaustAllowed.
    AtomicWord<size_t> _exhaustIsMasterConnections{0};
    // The number of connections whose last request was a hello with exhaustAllowed.
    AtomicWord<size_t> _exhaustHelloConnections{0};
};

}  // namespace mongo
