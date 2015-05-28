/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard.h"

#include <string>
#include <vector>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_runner.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::string;
    using std::stringstream;
    using std::vector;

namespace {

    class CmdGetShardMap : public Command {
    public:
        CmdGetShardMap() : Command( "getShardMap" ){}
        virtual void help( stringstream &help ) const { help<<"internal"; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getShardMap);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        virtual bool run(OperationContext* txn,
                         const string&,
                         mongo::BSONObj&,
                         int,
                         std::string& errmsg ,
                         mongo::BSONObjBuilder& result) {

            // MongoD instances do not know that they are part of a sharded cluster until they
            // receive a setShardVersion command and that's when the catalog manager and the shard
            // registry get initialized.
            if (grid.shardRegistry()) {
                grid.shardRegistry()->toBSON(&result);
            }

            return true;
        }

    } cmdGetShardMap;

} // namespace

    Shard::Shard(const ShardId& id,
                 const ConnectionString& connStr,
                 long long maxSizeMB,
                 bool isDraining)
        : _id(id),
          _cs(connStr),
          _maxSizeMB(maxSizeMB),
          _isDraining(isDraining) {

        invariant(_cs.isValid());
    }

    RemoteCommandTargeter* Shard::getTargeter() const {
        return grid.shardRegistry()->getTargeterForShard(getId()).get();
    }

    RemoteCommandRunner* Shard::getCommandRunner() const {
        return grid.shardRegistry()->getCommandRunner();
    }

    ShardPtr Shard::lookupRSName(const string& name) {
        return grid.shardRegistry()->lookupRSName(name);
    }

    BSONObj Shard::runCommand(const std::string& db, const std::string& simple) const {
        return runCommand(db, BSON(simple << 1));
    }

    BSONObj Shard::runCommand( const string& db , const BSONObj& cmd ) const {
        BSONObj res;
        bool ok = runCommand(db, cmd, res);
        if ( ! ok ) {
            stringstream ss;
            ss << "runCommand (" << cmd << ") on shard (" << _id << ") failed : " << res;
            throw UserException( 13136 , ss.str() );
        }
        res = res.getOwned();
        return res;
    }

    bool Shard::runCommand(const std::string& db, const std::string& simple, BSONObj& res) const {
        return runCommand(db, BSON(simple << 1), res);
    }

    bool Shard::runCommand(const string& db, const BSONObj& cmd, BSONObj& res) const {
        const ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly, TagSet::primaryOnly());
        auto selectedHost = getTargeter()->findHost(readPref);
        if (!selectedHost.isOK()) {
            return false;
        }

        const RemoteCommandRequest request(selectedHost.getValue(), db, cmd);

        auto statusCommand = getCommandRunner()->runCommand(request);
        if (!statusCommand.isOK()) {
            return false;
        }

        res = statusCommand.getValue().data.getOwned();

        return getStatusFromCommandResult(res).isOK();
    }

    ShardStatus Shard::getStatus() const {
        BSONObj listDatabases;
        uassert(28589,
                str::stream() << "call to listDatabases on " << getConnString().toString()
                              << " failed: " << listDatabases,
                runCommand("admin", BSON("listDatabases" << 1), listDatabases));

        BSONElement totalSizeElem = listDatabases["totalSize"];
        uassert(28590, "totalSize field not found in listDatabases", totalSizeElem.isNumber());

        BSONObj serverStatus;
        uassert(28591,
                str::stream() << "call to serverStatus on " << getConnString().toString()
                              << " failed: " << serverStatus,
                runCommand("admin", BSON("serverStatus" << 1), serverStatus));

        BSONElement versionElement = serverStatus["version"];
        uassert(28599, "version field not found in serverStatus", versionElement.type() == String);

        return ShardStatus(totalSizeElem.numberLong(), versionElement.str());
    }

    void Shard::reloadShardInfo() {
        grid.shardRegistry()->reload();
    }

    void Shard::removeShard(const ShardId& id) {
        grid.shardRegistry()->remove(id);
    }

    ShardPtr Shard::pick() {
        vector<ShardId> all;

        grid.shardRegistry()->getAllShardIds(&all);
        if (all.size() == 0) {
            grid.shardRegistry()->reload();
            grid.shardRegistry()->getAllShardIds(&all);

            if (all.empty()) {
                return nullptr;
            }
        }

        auto bestShard = grid.shardRegistry()->findIfExists(all[0]);
        if (!bestShard) {
            return nullptr;
        }

        ShardStatus bestStatus = bestShard->getStatus();

        for (size_t i = 1; i < all.size(); i++) {
            const auto& shard = grid.shardRegistry()->findIfExists(all[i]);
            if (!shard) {
                continue;
            }

            const ShardStatus status = shard->getStatus();

            if (status < bestStatus) {
                bestShard = shard;
                bestStatus = status;
            }
        }

        LOG(1) << "best shard for new allocation is " << bestStatus;
        return bestShard;
    }

    void Shard::installShard(const ShardId& id, const Shard& shard) {
        grid.shardRegistry()->set(id, shard);
    }

    ShardStatus::ShardStatus(long long dataSizeBytes, const string& mongoVersion)
        : _dataSizeBytes(dataSizeBytes),
          _mongoVersion(mongoVersion) {

    }

    std::string ShardStatus::toString() const {
        return str::stream() << " dataSizeBytes: " << _dataSizeBytes
                             << " version: " << _mongoVersion;
    }

    bool ShardStatus::operator< (const ShardStatus& other) const {
        return dataSizeBytes() < other.dataSizeBytes();
    }

} // namespace mongo
