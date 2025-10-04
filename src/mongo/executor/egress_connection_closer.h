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
