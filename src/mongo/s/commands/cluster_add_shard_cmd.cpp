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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/audit.h"
#include "mongo/db/commands.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {

class AddShardCmd : public Command {
public:
    AddShardCmd() : Command("addShard", false, "addshard") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "add a new shard to the system";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::addShard);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        // get replica set component hosts
        ConnectionString servers =
            ConnectionString::parse(cmdObj.firstElement().valuestrsafe(), errmsg);
        if (!errmsg.empty()) {
            log() << "addshard request " << cmdObj << " failed: " << errmsg;
            return false;
        }

        // using localhost in server names implies every other process must use localhost addresses
        // too
        std::vector<HostAndPort> serverAddrs = servers.getServers();
        for (size_t i = 0; i < serverAddrs.size(); i++) {
            if (serverAddrs[i].isLocalHost() != grid.allowLocalHost()) {
                errmsg = str::stream()
                    << "Can't use localhost as a shard since all shards need to"
                    << " communicate. Either use all shards and configdbs in localhost"
                    << " or all in actual IPs. host: " << serverAddrs[i].toString()
                    << " isLocalHost:" << serverAddrs[i].isLocalHost();

                log() << "addshard request " << cmdObj
                      << " failed: attempt to mix localhosts and IPs";
                return false;
            }

            // it's fine if mongods of a set all use default port
            if (!serverAddrs[i].hasPort()) {
                serverAddrs[i] =
                    HostAndPort(serverAddrs[i].host(), ServerGlobalParams::ShardServerPort);
            }
        }

        // name is optional; addShard will provide one if needed
        string name = "";
        if (cmdObj["name"].type() == String) {
            name = cmdObj["name"].valuestrsafe();
        }

        // maxSize is the space usage cap in a shard in MBs
        long long maxSize = 0;
        if (cmdObj[ShardType::maxSizeMB()].isNumber()) {
            maxSize = cmdObj[ShardType::maxSizeMB()].numberLong();
        }

        audit::logAddShard(ClientBasic::getCurrent(), name, servers.toString(), maxSize);

        StatusWith<string> addShardResult = grid.catalogManager()->addShard(
            txn, (name.empty() ? nullptr : &name), servers, maxSize);
        if (!addShardResult.isOK()) {
            log() << "addShard request '" << cmdObj << "'"
                  << " failed: " << addShardResult.getStatus().reason();
            return appendCommandStatus(result, addShardResult.getStatus());
        }

        result << "shardAdded" << addShardResult.getValue();

        return true;
    }

} addShard;


}  // namespace
}  // namespace mongo
