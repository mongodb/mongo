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

#include <map>
#include <string>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::unique_ptr;
using std::map;
using std::string;
using std::vector;

namespace {

class ListDatabasesCmd : public Command {
public:
    ListDatabasesCmd() : Command("listDatabases", true, "listdatabases") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "list databases in a cluster";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::listDatabases);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname_unused,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const bool nameOnly = cmdObj["nameOnly"].trueValue();

        map<string, long long> sizes;
        map<string, unique_ptr<BSONObjBuilder>> dbShardInfo;

        vector<ShardId> shardIds;
        grid.shardRegistry()->getAllShardIds(&shardIds);

        for (const ShardId& shardId : shardIds) {
            const auto shardStatus = grid.shardRegistry()->getShard(txn, shardId);
            if (!shardStatus.isOK()) {
                continue;
            }
            const auto s = shardStatus.getValue();

            auto response = uassertStatusOK(s->runCommandWithFixedRetryAttempts(
                txn,
                ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                "admin",
                cmdObj,
                Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(response.commandStatus);
            BSONObj x = std::move(response.response);

            BSONObjIterator j(x["databases"].Obj());
            while (j.more()) {
                BSONObj dbObj = j.next().Obj();

                const string name = dbObj["name"].String();
                const long long size = dbObj["sizeOnDisk"].numberLong();

                long long& sizeSumForDbAcrossShards = sizes[name];
                if (size == 1) {
                    if (sizeSumForDbAcrossShards <= 1) {
                        sizeSumForDbAcrossShards = 1;
                    }
                } else {
                    sizeSumForDbAcrossShards += size;
                }

                unique_ptr<BSONObjBuilder>& bb = dbShardInfo[name];
                if (!bb.get()) {
                    bb.reset(new BSONObjBuilder());
                }

                bb->appendNumber(s->getId().toString(), size);
            }
        }

        BSONArrayBuilder dbListBuilder(result.subarrayStart("databases"));
        for (map<string, long long>::iterator i = sizes.begin(); i != sizes.end(); ++i) {
            const string name = i->first;

            if (name == "local") {
                // We don't return local, since all shards have their own independent local
                continue;
            }

            if (name == "config" || name == "admin") {
                // Always get this from the config servers
                continue;
            }

            long long size = i->second;

            BSONObjBuilder temp;
            temp.append("name", name);
            if (!nameOnly) {
                temp.appendNumber("sizeOnDisk", size);
                temp.appendBool("empty", size == 1);
                temp.append("shards", dbShardInfo[name]->obj());
            }

            dbListBuilder.append(temp.obj());
        }

        // Get information for config and admin dbs from the config servers.
        auto catalogClient = grid.catalogClient(txn);
        auto appendStatus =
            catalogClient->appendInfoForConfigServerDatabases(txn, cmdObj, &dbListBuilder);
        if (!appendStatus.isOK()) {
            return Command::appendCommandStatus(result, appendStatus);
        }

        dbListBuilder.done();

        if (nameOnly)
            return true;

        // Compute the combined total size based on the response we've built so far.
        long long totalSize = 0;
        for (auto&& dbElt : result.asTempObj()["databases"].Obj()) {
            long long sizeOnDisk;
            uassertStatusOK(bsonExtractIntegerField(dbElt.Obj(), "sizeOnDisk"_sd, &sizeOnDisk));
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Found negative 'sizeOnDisk' in: " << dbElt.Obj(),
                    sizeOnDisk >= 0);
            totalSize += sizeOnDisk;
        }

        result.appendNumber("totalSize", totalSize);
        result.appendNumber("totalSizeMb", totalSize / (1024 * 1024));

        return true;
    }

} clusterCmdListDatabases;

}  // namespace
}  // namespace mongo
