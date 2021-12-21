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

#include <limits>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/transport/session.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * This is the entrypoint from the transport layer into mongod or mongos.
 *
 * The ServiceEntryPoint accepts new Sessions from the TransportLayer, and is
 * responsible for running these Sessions in a get-Message, run-Message,
 * reply-with-Message loop.  It may not do this on the TransportLayer’s thread.
 */
class ServiceEntryPoint {
    ServiceEntryPoint(const ServiceEntryPoint&) = delete;
    ServiceEntryPoint& operator=(const ServiceEntryPoint&) = delete;

public:
    virtual ~ServiceEntryPoint() = default;

    /**
     * Begin running a new Session. This method returns immediately.
     */
    virtual void startSession(transport::SessionHandle session) = 0;

    /**
     * End all sessions that do not match the mask in tags.
     */
    virtual void endAllSessions(transport::Session::TagMask tags) = 0;

    /**
     * Starts the service entry point
     */
    virtual Status start() = 0;

    /**
     * Shuts down the service entry point.
     */
    virtual bool shutdown(Milliseconds timeout) = 0;

    /**
     * Append high-level stats to a BSONObjBuilder for serverStatus
     */
    virtual void appendStats(BSONObjBuilder* bob) const = 0;

    /**
     * Returns the number of sessions currently open.
     */
    virtual size_t numOpenSessions() const = 0;

    /**
     * Returns the maximum number of sessions that can be open.
     */
    virtual size_t maxOpenSessions() const {
        return std::numeric_limits<size_t>::max();
    }

    /**
     * Processes a request and fills out a DbResponse.
     */
    virtual Future<DbResponse> handleRequest(OperationContext* opCtx,
                                             const Message& request) noexcept = 0;

    /**
     * Optional handler which is invoked after a session ends.
     *
     * This function implies that the Session itself will soon be destructed.
     */
    virtual void onEndSession(const transport::SessionHandle&) {}

    /**
     * Optional handler which is invoked after a client connects.
     */
    virtual void onClientConnect(Client* client) {}

    /**
     * Optional handler which is invoked after a client disconnect. A client disconnect occurs when
     * the connection between the mongo process and client is closed for any reason, and is defined
     * by the destruction and cleanup of the ServiceStateMachine that manages the client.
     */
    virtual void onClientDisconnect(Client* client) {}

protected:
    ServiceEntryPoint() = default;
};

}  // namespace mongo
