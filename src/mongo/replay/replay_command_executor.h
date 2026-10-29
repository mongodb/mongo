// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/util/modules.h"

#include <chrono>
#include <memory>
#include <string_view>
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
    /*
     * Connect the executor to the server instance passed in the constructor. The connection status
     * is checked and if successful a new client instance is created.
     */
    void connect(std::string_view uri);
    /*
     * Reset the connection, this method is particularly useful if a new connection needs to be
     * established without creating a new instance of this class.
     */
    void reset();
    /**
     * Simply checks if the executor is connected to same instance.
     */
    bool isConnected() const;
    /*
     * Given a well formed binary protocol bson command encapsulated inside a replay command. This
     * method runs the command against the server (if connection is established).
     */
    BSONObj runCommand(const ReplayCommand& command) const;

private:
    std::unique_ptr<DBClientBase> _dbConnection = nullptr;
};
}  // namespace mongo
