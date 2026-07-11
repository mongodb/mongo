// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/session.h"
#include "mongo/util/net/hostandport.h"

#include <functional>

namespace mongo {
namespace executor {

/**
 * Thin interface for types that can drop connections, or drop connections considering only those
 * associated with a HostAndPort. Connections associated with a HostAndPort can also be marked as
 * to be kept open, though it is up to implementations to decide on the interactions between
 * dropping and keeping open.
 *
 * Largely to support egress connections on upgrade/downgrade scenarios with hosts we should now
 * no longer talk to.
 */
class EgressConnectionCloser {
public:
    virtual ~EgressConnectionCloser() {}

    /**
     * Drop connections to all targets not marked keep-open and relay a status message describing
     * why the connection was dropped.
     */
    virtual void dropConnections(const Status& status) = 0;
    void dropConnections() {
        dropConnections(Status(ErrorCodes::PooledConnectionsDropped, "Drop all connections"));
    }

    /**
     * Drop connections to a specific target and relay a status message describing why the
     * connection was dropped.
     */
    virtual void dropConnections(const HostAndPort& target, const Status& status) = 0;
    void dropConnections(const HostAndPort& target) {
        dropConnections(
            target,
            Status(ErrorCodes::PooledConnectionsDropped, "Drop connections to a specific target"));
    }

    // Mark connections associated with a certain HostAndPort as to be kept open.
    virtual void setKeepOpen(const HostAndPort& target, bool keepOpen) = 0;

protected:
    EgressConnectionCloser() {}
};

}  // namespace executor
}  // namespace mongo
