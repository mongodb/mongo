/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"

#include <chrono>
#include <memory>
#include <vector>

namespace mongo {
class ServiceContext;
class ReplayCommand;

/*
 *   ReplayCommandExecutor is not an object to create and throw away constantly.
 *   It is meant to be used for serving a client session for all the queries
 *   received in a recording.
 */
class ReplayCommandExecutor {
public:
    /** Initialize the replay executor. This method must always be called. */
    bool init();
    /*
     * Connect the executor to the server instance passed in the constructor. The connection status
     * is checked and if successful a new client instance is created.
     */
    void connect(StringData uri);
    /*
     * Reset the connection, this method is particularly useful if a new connection needs to be
     * established without creating a new instance of this class.
     */
    void reset();
    /** Simply checks if the executor is connected to same instance. */
    bool isConnected() const;
    /*
     * Given a well formed binary protocol bson command encapsulated inside a replay command. This
     * method runs the command against the server (if connection is established).
     */
    BSONObj runCommand(const ReplayCommand&) const;

private:
    void setup() const;
    void setupTransportLayer(ServiceContext&) const;
    void setupWireProtocol(ServiceContext&) const;
    std::unique_ptr<DBClientBase> _dbConnection = nullptr;
};
}  // namespace mongo
