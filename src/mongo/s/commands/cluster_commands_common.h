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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class BSONObj;

class AScopedConnection;
class ClusterCursorManager;
class DBClientBase;
class DBClientCursor;

namespace executor {
class TaskExecutor;
}  // namespace executor

/**
 * DEPRECATED - do not use in any new code. All new code must use the TaskExecutor interface
 * instead.
 */
class Future {
public:
    class CommandResult {
    public:
        std::string getServer() const {
            return _server;
        }

        bool isDone() const {
            return _done;
        }

        bool ok() const {
            verify(_done);
            return _ok;
        }

        BSONObj result() const {
            verify(_done);
            return _res;
        }

        /**
           blocks until command is done
           returns ok()
         */
        bool join(OperationContext* txn, int maxRetries = 1);

    private:
        CommandResult(const std::string& server,
                      const std::string& db,
                      const BSONObj& cmd,
                      int options,
                      DBClientBase* conn,
                      bool useShardedConn);
        void init();

        std::string _server;
        std::string _db;
        int _options;
        BSONObj _cmd;
        DBClientBase* _conn;
        std::unique_ptr<AScopedConnection> _connHolder;  // used if not provided a connection
        bool _useShardConn;

        std::unique_ptr<DBClientCursor> _cursor;

        BSONObj _res;
        bool _ok;
        bool _done;

        friend class Future;
    };


    /**
     * @param server server name
     * @param db db name
     * @param cmd cmd to exec
     * @param conn optional connection to use.  will use standard pooled if non-specified
     * @param useShardConn use ShardConnection
     */
    static std::shared_ptr<CommandResult> spawnCommand(const std::string& server,
                                                       const std::string& db,
                                                       const BSONObj& cmd,
                                                       int options,
                                                       DBClientBase* conn = 0,
                                                       bool useShardConn = false);
};

/**
 * Utility function to compute a single error code from a vector of command results.
 *
 * @return If there is an error code common to all of the error results, returns that error
 *          code; otherwise, returns 0.
 */
int getUniqueCodeFromCommandResults(const std::vector<Strategy::CommandResult>& results);

/**
 * Utility function to return an empty result set from a command.
 */
bool appendEmptyResultSet(BSONObjBuilder& result, Status status, const std::string& ns);

}  // namespace mongo
