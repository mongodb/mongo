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

#include <boost/scoped_ptr.hpp>

#include <map>
#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/commands.h"
#include "mongo/s/client/shard.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::map;
    using std::string;
    using std::vector;

namespace {

    class ListDatabasesCmd : public Command {
    public:
        ListDatabasesCmd() : Command("listDatabases", true, "listdatabases") { }

        virtual bool slaveOk() const {
            return true;
        }

        virtual bool slaveOverrideOk() const {
            return true;
        }

        virtual bool adminOnly() const {
            return true;
        }

        virtual bool isWriteCommandForConfigServer() const {
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
                         BSONObjBuilder& result,
                         bool fromRepl) {

            vector<Shard> shards;
            Shard::getAllShards(shards);

            map<string, long long> sizes;
            map<string, scoped_ptr<BSONObjBuilder> > dbShardInfo;

            for (vector<Shard>::iterator i = shards.begin(); i != shards.end(); i++) {
                Shard s = *i;
                BSONObj x = s.runCommand("admin", "listDatabases");

                BSONObjIterator j(x["databases"].Obj());
                while (j.more()) {
                    BSONObj dbObj = j.next().Obj();

                    const string name = dbObj["name"].String();
                    const long long size = dbObj["sizeOnDisk"].numberLong();

                    long long& totalSize = sizes[name];
                    if (size == 1) {
                        if (totalSize <= 1) {
                            totalSize = 1;
                        }
                    }
                    else {
                        totalSize += size;
                    }

                    scoped_ptr<BSONObjBuilder>& bb = dbShardInfo[name];
                    if (!bb.get()) {
                        bb.reset(new BSONObjBuilder());
                    }

                    bb->appendNumber(s.getName(), size);
                }

            }

            long long totalSize = 0;

            BSONArrayBuilder bb(result.subarrayStart("databases"));
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
                totalSize += size;

                BSONObjBuilder temp;
                temp.append("name", name);
                temp.appendNumber("sizeOnDisk", size);
                temp.appendBool("empty", size == 1);
                temp.append("shards", dbShardInfo[name]->obj());

                bb.append(temp.obj());
            }

            // obtain cached config shard
            Shard configShard = Shard::findIfExists("config");
            if (!configShard.ok()) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::ShardNotFound,
                                                  "Couldn't find shard "
                                                  "representing config server"));
            }

            {
                // get config db from the config servers (first one)
                BSONObj x;
                if (configShard.runCommand("config", "dbstats", x)) {
                    BSONObjBuilder b;
                    b.append("name", "config");
                    b.appendBool("empty", false);
                    if (x["fileSize"].type())
                        b.appendAs(x["fileSize"], "sizeOnDisk");
                    else
                        b.append("sizeOnDisk", 1);
                    bb.append(b.obj());
                }
                else {
                    bb.append(BSON("name" << "config"));
                }
            }

            {
                // get admin db from the config servers (first one)
                BSONObj x;
                if (configShard.runCommand("admin", "dbstats", x)) {
                    BSONObjBuilder b;
                    b.append("name", "admin");
                    b.appendBool("empty", false);

                    if (x["fileSize"].type()) {
                        b.appendAs(x["fileSize"], "sizeOnDisk");
                    }
                    else {
                        b.append("sizeOnDisk", 1);
                    }

                    bb.append(b.obj());
                }
                else {
                    bb.append(BSON("name" << "admin"));
                }
            }

            bb.done();

            result.appendNumber("totalSize", totalSize);
            result.appendNumber("totalSizeMb", totalSize / (1024 * 1024));

            return 1;
        }

    } cmdListDatabases;

} // namespace
} // namespace mongo
