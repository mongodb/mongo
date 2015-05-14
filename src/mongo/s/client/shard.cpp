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

#include <boost/make_shared.hpp>
#include <set>
#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::list;
    using std::map;
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

            grid.shardRegistry()->toBSON(&result);
            return true;
        }

    } cmdGetShardMap;

} // namespace


    Shard::Shard()
        : _name(""),
          _addr(""),
          _maxSizeMB(0),
          _isDraining(false) {

    }

    Shard::Shard(const std::string& name,
                 const std::string& addr,
                 long long maxSizeMB,
                 bool isDraining)
        : _name(name),
          _addr(addr),
          _maxSizeMB(maxSizeMB),
          _isDraining(isDraining) {

        _setAddr(addr);
    }

    Shard::Shard(const std::string& name,
                 const ConnectionString& connStr,
                 long long maxSizeMB,
                 bool isDraining)
        : _name(name),
          _addr(connStr.toString()),
          _cs(connStr),
          _maxSizeMB(maxSizeMB),
          _isDraining(isDraining) {

    }

    Shard Shard::findIfExists( const string& shardName ) {
        ShardPtr shard = grid.shardRegistry()->findIfExists( shardName );
        return shard ? *shard : Shard::EMPTY;
    }

    void Shard::_setAddr( const string& addr ) {
        _addr = addr;
        if ( !_addr.empty() ) {
            _cs = ConnectionString( addr , ConnectionString::SET );
        }
    }

    void Shard::reset( const string& ident ) {
        *this = grid.shardRegistry()->findCopy( ident );
    }

    bool Shard::containsNode( const string& node ) const {
        if ( _addr == node )
            return true;

        if ( _cs.type() == ConnectionString::SET ) {
            ReplicaSetMonitorPtr rs = ReplicaSetMonitor::get( _cs.getSetName(), true );

            if (!rs) {
                // Possibly still yet to be initialized. See SERVER-8194.
                warning() << "Monitor not found for a known shard: " << _cs.getSetName();
                return false;
            }

            return rs->contains(HostAndPort(node));
        }

        return false;
    }

    void Shard::getAllShards( vector<Shard>& all ) {
        grid.shardRegistry()->getAllShards( all );
    }

    bool Shard::isAShardNode( const string& ident ) {
        return grid.shardRegistry()->isAShardNode( ident );
    }

    Shard Shard::lookupRSName( const string& name) {
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
            ss << "runCommand (" << cmd << ") on shard (" << _name << ") failed : " << res;
            throw UserException( 13136 , ss.str() );
        }
        res = res.getOwned();
        return res;
    }

    bool Shard::runCommand(const std::string& db, const std::string& simple, BSONObj& res) const {
        return runCommand(db, BSON(simple << 1), res);
    }

    bool Shard::runCommand(const string& db, const BSONObj& cmd, BSONObj& res) const {
        ScopedDbConnection conn(getConnString());
        bool ok = conn->runCommand(db, cmd, res);
        conn.done();
        return ok;
    }

    string Shard::getShardMongoVersion(const string& shardHost) {
        ScopedDbConnection conn(shardHost);
        BSONObj serverStatus;
        bool ok = conn->runCommand("admin", BSON("serverStatus" << 1), serverStatus);
        conn.done();

        uassert(28598,
                str::stream() << "call to serverStatus on " << shardHost
                              << " failed: " << serverStatus,
                ok);

        BSONElement versionElement = serverStatus["version"];

        uassert(28589, "version field not found in serverStatus",
                versionElement.type() == String);
        return serverStatus["version"].String();
    }

    long long Shard::getShardDataSizeBytes(const string& shardHost) {
        ScopedDbConnection conn(shardHost);
        BSONObj listDatabases;
        bool ok = conn->runCommand("admin", BSON("listDatabases" << 1), listDatabases);
        conn.done();

        uassert(28599,
                str::stream() << "call to listDatabases on " << shardHost
                              << " failed: " << listDatabases,
                ok);

        BSONElement totalSizeElem = listDatabases["totalSize"];

        uassert(28590, "totalSize field not found in listDatabases",
                totalSizeElem.isNumber());
        return listDatabases["totalSize"].numberLong();
    }

    ShardStatus Shard::getStatus() const {
        return ShardStatus(*this,
                           getShardDataSizeBytes(getConnString()),
                           getShardMongoVersion(getConnString()));
    }

    void Shard::reloadShardInfo() {
        grid.shardRegistry()->reload();
    }


    void Shard::removeShard( const string& name ) {
        grid.shardRegistry()->remove( name );
    }

    Shard Shard::pick( const Shard& current ) {
        vector<Shard> all;
        grid.shardRegistry()->getAllShards( all );
        if ( all.size() == 0 ) {
            grid.shardRegistry()->reload();
            grid.shardRegistry()->getAllShards( all );
            if ( all.size() == 0 )
                return EMPTY;
        }

        // if current shard was provided, pick a different shard only if it is a better choice
        ShardStatus best = all[0].getStatus();
        if ( current != EMPTY ) {
            best = current.getStatus();
        }

        for ( size_t i=0; i<all.size(); i++ ) {
            ShardStatus t = all[i].getStatus();
            if ( t < best )
                best = t;
        }

        LOG(1) << "best shard for new allocation is " << best;
        return best.shard();
    }

    void Shard::installShard(const std::string& name, const Shard& shard) {
        grid.shardRegistry()->set(name, shard);
    }

    ShardStatus::ShardStatus(const Shard& shard, long long dataSizeBytes, const string& version):
            _shard(shard), _dataSizeBytes(dataSizeBytes), _mongoVersion(version) {
    }

} // namespace mongo
