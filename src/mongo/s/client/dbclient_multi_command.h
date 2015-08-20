/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include <deque>

#include "mongo/bson/bsonobj.h"
#include "mongo/s/client/multi_command_dispatch.h"

namespace mongo {

class ShardConnection;

/**
 * A DBClientMultiCommand uses the client driver (DBClientConnections) to send and recv
 * commands to different hosts in parallel.
 *
 * See MultiCommandDispatch for more details.
 */
class DBClientMultiCommand : public MultiCommandDispatch {
public:
    /**
     * Specifies whether this multi command instance will be used for talking to the config server,
     * in which case, the dispatcher will not attempt to do the set shard version initialization.
     */
    DBClientMultiCommand(bool isConfig = false);

    ~DBClientMultiCommand();

    void addCommand(const ConnectionString& endpoint,
                    StringData dbName,
                    const BSONObj& request) override;

    void sendAll() override;

    int numPending() const override;

    Status recvAny(ConnectionString* endpoint, BSONSerializable* response) override;

private:
    // All info associated with an pre- or in-flight command
    struct PendingCommand {
        PendingCommand(const ConnectionString& endpoint, StringData dbName, const BSONObj& cmdObj);
        ~PendingCommand();

        // What to send
        const ConnectionString endpoint;
        const std::string dbName;
        const BSONObj cmdObj;

        // Where to send it
        std::unique_ptr<ShardConnection> conn;

        // If anything goes wrong
        Status status;
    };

    typedef std::deque<PendingCommand*> PendingQueue;

    const bool _isConfig;

    PendingQueue _pendingCommands;
};

}  // namespace mongo
