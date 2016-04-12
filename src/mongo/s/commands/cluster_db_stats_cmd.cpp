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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/s/commands/run_on_all_shards_cmd.h"

namespace mongo {
namespace {

using std::vector;

class DBStatsCmd : public RunOnAllShardsCommand {
public:
    DBStatsCmd() : RunOnAllShardsCommand("dbStats", "dbstats") {}
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::dbStats);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void aggregateResults(const vector<ShardAndReply>& results, BSONObjBuilder& output) {
        long long objects = 0;
        long long unscaledDataSize = 0;
        long long dataSize = 0;
        long long storageSize = 0;
        long long numExtents = 0;
        long long indexes = 0;
        long long indexSize = 0;
        long long fileSize = 0;

        long long freeListNum = 0;
        long long freeListSize = 0;

        for (const ShardAndReply& shardAndReply : results) {
            const BSONObj& b = std::get<1>(shardAndReply);

            objects += b["objects"].numberLong();
            unscaledDataSize += b["avgObjSize"].numberLong() * b["objects"].numberLong();
            dataSize += b["dataSize"].numberLong();
            storageSize += b["storageSize"].numberLong();
            numExtents += b["numExtents"].numberLong();
            indexes += b["indexes"].numberLong();
            indexSize += b["indexSize"].numberLong();
            fileSize += b["fileSize"].numberLong();

            if (b["extentFreeList"].isABSONObj()) {
                freeListNum += b["extentFreeList"].Obj()["num"].numberLong();
                freeListSize += b["extentFreeList"].Obj()["totalSize"].numberLong();
            }
        }

        // TODO: need to find a good way to get this
        // result.appendNumber( "collections" , ncollections );
        output.appendNumber("objects", objects);

        // avgObjSize on mongod is not scaled based on the argument to db.stats(), so we use
        // unscaledDataSize here for consistency.  See SERVER-7347.
        output.append("avgObjSize", objects == 0 ? 0 : double(unscaledDataSize) / double(objects));
        output.appendNumber("dataSize", dataSize);
        output.appendNumber("storageSize", storageSize);
        output.appendNumber("numExtents", numExtents);
        output.appendNumber("indexes", indexes);
        output.appendNumber("indexSize", indexSize);
        output.appendNumber("fileSize", fileSize);

        {
            BSONObjBuilder extentFreeList(output.subobjStart("extentFreeList"));
            extentFreeList.appendNumber("num", freeListNum);
            extentFreeList.appendNumber("totalSize", freeListSize);
            extentFreeList.done();
        }
    }

} clusterDBStatsCmd;

}  // namespace
}  // namespace mongo
