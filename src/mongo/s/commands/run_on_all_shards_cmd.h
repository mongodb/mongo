/**
*    Copyright (C) 2015 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include <string>
#include <tuple>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class OperationContext;

/**
 * Logic for commands that simply map out to all shards then fold the results into
 * a single response.
 *
 * All shards are contacted in parallel.
 *
 * When extending, don't override run() - but rather aggregateResults(). If you need
 * to implement some kind of fall back logic for multiversion clusters,
 * override specialErrorHandler().
 */
class RunOnAllShardsCommand : public Command {
public:
    RunOnAllShardsCommand(const char* name,
                          const char* oldName = NULL,
                          bool useShardConn = false,
                          bool implicitCreateDb = false);

    bool slaveOk() const override {
        return true;
    }
    bool adminOnly() const override {
        return false;
    }

    // The StringData contains the shard ident.
    // This can be used to create an instance of Shard
    using ShardAndReply = std::tuple<StringData, BSONObj>;

    virtual void aggregateResults(const std::vector<ShardAndReply>& results,
                                  BSONObjBuilder& output);

    // The default implementation is the identity function.
    virtual BSONObj specialErrorHandler(const std::string& server,
                                        const std::string& db,
                                        const BSONObj& cmdObj,
                                        const BSONObj& originalResult) const;

    // The default implementation uses all shards.
    virtual void getShardIds(OperationContext* txn,
                             const std::string& db,
                             BSONObj& cmdObj,
                             std::vector<ShardId>& shardIds);

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& output) final;

private:
    // Use ShardConnection as opposed to ScopedDbConnection
    const bool _useShardConn;

    // Whether the requested database should be created implicitly
    const bool _implicitCreateDb;
};

}  // namespace mongo
