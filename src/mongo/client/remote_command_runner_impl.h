/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/client/connection_pool.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"

namespace mongo {

namespace executor {
class NetworkConnectionHook;
}  // namespace executor

template <typename T>
class StatusWith;

/**
 * Utility used by the network executor to run commands against a MongoDB instance. It abstracts
 * the logic of managing connections and turns the remote instance into a stateless
 * request-response service.
 */
class RemoteCommandRunnerImpl {
public:
    RemoteCommandRunnerImpl(int messagingTags,
                            std::unique_ptr<executor::NetworkConnectionHook> hook);
    ~RemoteCommandRunnerImpl();

    /**
     * Starts up the connection pool.
     */
    void startup();

    /**
     * Closes all underlying connections. Must be called before the destructor runs.
     */
    void shutdown();

    /**
     * Synchronously invokes the command described by "request" and returns the server's
     * response or any status.
     */
    StatusWith<executor::RemoteCommandResponse> runCommand(
        const executor::RemoteCommandRequest& request);

private:
    // The connection pool on which to send requests
    ConnectionPool _connPool;

    // True if startup has been called
    bool _active{false};
};

}  // namespace mongo
