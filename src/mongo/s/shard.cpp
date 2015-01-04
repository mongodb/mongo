// shard.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/s/shard.h"

#include <boost/make_shared.hpp>
#include <set>
#include <string>
#include <vector>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/request.h"
#include "mongo/s/scc_fast_query_handler.h"
#include "mongo/s/type_shard.h"
#include "mongo/s/version_manager.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

    static bool initWireVersion( DBClientBase* conn, std::string* errMsg ) {
        BSONObj response;
        if ( !conn->runCommand( "admin", BSON("isMaster" << 1), response )) {
            *errMsg = str::stream() << "Failed to determine wire version "
                                    << "for internal connection: " << response;
            return false;
        }

        if ( response.hasField("minWireVersion") && response.hasField("maxWireVersion") ) {
            int minWireVersion = response["minWireVersion"].numberInt();
            int maxWireVersion = response["maxWireVersion"].numberInt();
            conn->setWireVersions( minWireVersion, maxWireVersion );
        }

        return true;
    }

    class StaticShardInfo {
    public:
        StaticShardInfo() : _mutex("StaticShardInfo"), _rsMutex("RSNameMap") { }
        void reload() {

            list<BSONObj> all;
            {
                ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);
                auto_ptr<DBClientCursor> c = conn->query(ShardType::ConfigNS , Query());
                massert( 13632 , "couldn't get updated shard list from config server" , c.get() );

                int numShards = 0;
                while ( c->more() ) {
                    all.push_back( c->next().getOwned() );
                    ++numShards;
                }

                LOG( 1 ) << "found " << numShards << " shards listed on config server(s): "
                         << conn.get()->toString() << endl;

                conn.done();
            }

            scoped_lock lk( _mutex );

            // We use the _lookup table for all shards and for the primary config DB. The config DB info,
            // however, does not come from the ShardNS::shard. So when cleaning the _lookup table we leave
            // the config state intact. The rationale is that this way we could drop shards that
            // were removed without reinitializing the config DB information.

            ShardMap::iterator i = _lookup.find( "config" );
            if ( i != _lookup.end() ) {
                ShardPtr config = i->second;
                _lookup.clear();
                _lookup[ "config" ] = config;
            }
            else {
                _lookup.clear();
            }
            _rsLookup.clear();
            
            for (list<BSONObj>::const_iterator iter = all.begin(); iter != all.end(); ++iter) {
                ShardType shardData;

                string errmsg;
                if (!shardData.parseBSON(*iter, &errmsg) || !shardData.isValid(&errmsg)) {
                    uasserted(28530, errmsg);
                }

                const long long maxSize = shardData.isMaxSizeSet() ? shardData.getMaxSize() : 0;
                const bool isDraining = shardData.isDrainingSet() ?
                        shardData.getDraining() : false;
                const BSONArray tags = shardData.isTagsSet() ? shardData.getTags() : BSONArray();
                ShardPtr shard = boost::make_shared<Shard>(shardData.getName(),
                                                           shardData.getHost(),
                                                           maxSize,
                                                           isDraining,
                                                           tags);

                _lookup[shardData.getName()] = shard;
                _installHost(shardData.getHost(), shard);
            }

        }

        ShardPtr findIfExists( const string& shardName ) {
            scoped_lock lk( _mutex );
            ShardMap::iterator i = _lookup.find( shardName );
            if ( i != _lookup.end() ) return i->second;
            return ShardPtr();
        }

        ShardPtr find(const string& ident) {
            string errmsg;
            ConnectionString connStr = ConnectionString::parse(ident, errmsg);

            uassert(18642, str::stream() << "Error parsing connection string: " << ident,
                    errmsg.empty());

            if (connStr.type() == ConnectionString::SET) {
                scoped_lock lk(_rsMutex);
                ShardMap::iterator iter = _rsLookup.find(connStr.getSetName());

                if (iter == _rsLookup.end()) {
                    return ShardPtr();
                }

                return iter->second;
            }
            else {
                scoped_lock lk(_mutex);
                ShardMap::iterator iter = _lookup.find(ident);

                if (iter == _lookup.end()) {
                    return ShardPtr();
                }

                return iter->second;
            }
        }

        ShardPtr findWithRetry(const string& ident) {
            ShardPtr shard(find(ident));

            if (shard != NULL) {
                return shard;
            }

            // not in our maps, re-load all
            reload();

            shard = find(ident);
            massert(13129 , str::stream() << "can't find shard for: " << ident, shard != NULL);
            return shard;
        }

        // Lookup shard by replica set name. Returns Shard::EMTPY if the name can't be found.
        // Note: this doesn't refresh the table if the name isn't found, so it's possible that
        // a newly added shard/Replica Set may not be found.
        Shard lookupRSName( const string& name) {
            scoped_lock lk( _rsMutex );
            ShardMap::iterator i = _rsLookup.find( name );

            return (i == _rsLookup.end()) ? Shard::EMPTY : *(i->second.get());
        }

        // Useful for ensuring our shard data will not be modified while we use it
        Shard findCopy( const string& ident ){
            ShardPtr found = findWithRetry(ident);
            scoped_lock lk( _mutex );
            massert( 13128 , (string)"can't find shard for: " + ident , found.get() );
            return *found.get();
        }

        void set( const string& name , const Shard& s , bool setName = true , bool setAddr = true ) {
            scoped_lock lk( _mutex );
            ShardPtr ss( new Shard( s ) );
            if ( setName )
                _lookup[name] = ss;
            if ( setAddr )
                _installHost( s.getConnString() , ss );
        }

        void _installHost( const string& host , const ShardPtr& s ) {
            _lookup[host] = s;

            const ConnectionString& cs = s->getAddress();
            if ( cs.type() == ConnectionString::SET ) {
                if ( cs.getSetName().size() ) {
                    scoped_lock lk( _rsMutex);
                    _rsLookup[ cs.getSetName() ] = s;
                }
                vector<HostAndPort> servers = cs.getServers();
                for ( unsigned i=0; i<servers.size(); i++ ) {
                    _lookup[ servers[i].toString() ] = s;
                }
            }
        }

        void remove( const string& name ) {
            scoped_lock lk( _mutex );
            for ( ShardMap::iterator i = _lookup.begin(); i!=_lookup.end(); ) {
                ShardPtr s = i->second;
                if ( s->getName() == name ) {
                    _lookup.erase(i++);
                }
                else {
                    ++i;
                }
            }
            for ( ShardMap::iterator i = _rsLookup.begin(); i!=_rsLookup.end(); ) {
                ShardPtr s = i->second;
                if ( s->getName() == name ) {
                    _rsLookup.erase(i++);
                }
                else {
                    ++i;
                }
            }
        }

        void getAllShards( vector<ShardPtr>& all ) const {
            scoped_lock lk( _mutex );
            std::set<string> seen;
            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                const ShardPtr& s = i->second;
                if ( s->getName() == "config" )
                    continue;
                if ( seen.count( s->getName() ) )
                    continue;
                seen.insert( s->getName() );
                all.push_back( s );
            }
        }

        void getAllShards( vector<Shard>& all ) const {
            scoped_lock lk( _mutex );
            std::set<string> seen;
            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                const ShardPtr& s = i->second;
                if ( s->getName() == "config" )
                    continue;
                if ( seen.count( s->getName() ) )
                    continue;
                seen.insert( s->getName() );
                all.push_back( *s );
            }
        }


        bool isAShardNode( const string& addr ) const {
            scoped_lock lk( _mutex );

            // check direct nods or set names
            ShardMap::const_iterator i = _lookup.find( addr );
            if ( i != _lookup.end() )
                return true;

            // check for set nodes
            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                if ( i->first == "config" )
                    continue;

                if ( i->second->containsNode( addr ) )
                    return true;
            }

            return false;
        }

        bool getShardMap( BSONObjBuilder& result , string& errmsg ) const {
            scoped_lock lk( _mutex );

            BSONObjBuilder b( _lookup.size() + 50 );

            for ( ShardMap::const_iterator i = _lookup.begin(); i!=_lookup.end(); ++i ) {
                b.append( i->first , i->second->getConnString() );
            }

            result.append( "map" , b.obj() );

            return true;
        }

    private:
        typedef map<string,ShardPtr> ShardMap;
        ShardMap _lookup; // Map of both shardName -> Shard and hostName -> Shard
        ShardMap _rsLookup; // Map from ReplSet name to shard
        mutable mongo::mutex _mutex;
        mutable mongo::mutex _rsMutex;
    } staticShardInfo;


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
        virtual bool run(OperationContext* txn, const string&, mongo::BSONObj&, int, std::string& errmsg , mongo::BSONObjBuilder& result, bool) {
            return staticShardInfo.getShardMap( result , errmsg );
        }
    } cmdGetShardMap;

    Shard::Shard(const std::string& name,
            const std::string& addr,
            long long maxSizeMB,
            bool isDraining,
            const BSONArray& tags):
                _name(name),
                _addr(addr),
                _maxSizeMB(maxSizeMB),
                _isDraining(isDraining) {
        _setAddr(addr);

        BSONArrayIteratorSorted iter(tags);
        while (iter.more()) {
            BSONElement tag = iter.next();
            _tags.insert(tag.String());
        }
    }

    Shard::Shard(const std::string& name,
            const ConnectionString& connStr,
            long long maxSizeMB,
            bool isDraining,
            const set<string>& tags):
                _name(name),
                _addr(connStr.toString()),
                _cs(connStr),
                _maxSizeMB(maxSizeMB),
                _isDraining(isDraining),
                _tags(tags) {
    }

    Shard Shard::findIfExists( const string& shardName ) {
        ShardPtr shard = staticShardInfo.findIfExists( shardName );
        return shard ? *shard : Shard::EMPTY;
    }

    void Shard::_setAddr( const string& addr ) {
        _addr = addr;
        if ( !_addr.empty() ) {
            _cs = ConnectionString( addr , ConnectionString::SET );
        }
    }

    void Shard::reset( const string& ident ) {
        *this = staticShardInfo.findCopy( ident );
    }

    bool Shard::containsNode( const string& node ) const {
        if ( _addr == node )
            return true;

        if ( _cs.type() == ConnectionString::SET ) {
            ReplicaSetMonitorPtr rs = ReplicaSetMonitor::get( _cs.getSetName(), true );

            if (!rs) {
                // Possibly still yet to be initialized. See SERVER-8194.
                warning() << "Monitor not found for a known shard: " << _cs.getSetName() << endl;
                return false;
            }

            return rs->contains(HostAndPort(node));
        }

        return false;
    }

    void Shard::getAllShards( vector<Shard>& all ) {
        staticShardInfo.getAllShards( all );
    }

    bool Shard::isAShardNode( const string& ident ) {
        return staticShardInfo.isAShardNode( ident );
    }

    Shard Shard::lookupRSName( const string& name) {
        return staticShardInfo.lookupRSName(name);
    }

    void Shard::printShardInfo( ostream& out ) {
        vector<Shard> all;
        staticShardInfo.getAllShards( all );
        for ( unsigned i=0; i<all.size(); i++ )
            out << all[i].toString() << "\n";
        out.flush();
    }

    BSONObj Shard::runCommand( const string& db , const BSONObj& cmd ) const {
        ScopedDbConnection conn(getConnString());
        BSONObj res;
        bool ok = conn->runCommand( db , cmd , res );
        if ( ! ok ) {
            stringstream ss;
            ss << "runCommand (" << cmd << ") on shard (" << _name << ") failed : " << res;
            conn.done();
            throw UserException( 13136 , ss.str() );
        }
        res = res.getOwned();
        conn.done();
        return res;
    }

    ShardStatus Shard::getStatus() const {
        BSONObj serverStatus = runCommand("admin", BSON("serverStatus" << 1));
        BSONElement versionElement = serverStatus["version"];

        uassert(28589, "version field not found in serverStatus",
                versionElement.type() == String);
        string version = serverStatus["version"].String();

        BSONObj listDatabases = runCommand("admin", BSON("listDatabases" << 1));
        BSONElement totalSizeElem = listDatabases["totalSize"];

        uassert(28590, "totalSize field not found in listDatabases",
                totalSizeElem.isNumber());
        long long dataSizeBytes = listDatabases["totalSize"].numberLong();

        return ShardStatus(*this, dataSizeBytes, version);
    }

    void Shard::reloadShardInfo() {
        staticShardInfo.reload();
    }


    void Shard::removeShard( const string& name ) {
        staticShardInfo.remove( name );
    }

    Shard Shard::pick( const Shard& current ) {
        vector<Shard> all;
        staticShardInfo.getAllShards( all );
        if ( all.size() == 0 ) {
            staticShardInfo.reload();
            staticShardInfo.getAllShards( all );
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

        LOG(1) << "best shard for new allocation is " << best << endl;
        return best.shard();
    }

    void Shard::installShard(const std::string& name, const Shard& shard) {
        staticShardInfo.set(name, shard, true, false);
    }

    ShardStatus::ShardStatus(const Shard& shard, long long dataSizeBytes, const string& version):
            _shard(shard), _dataSizeBytes(dataSizeBytes), _mongoVersion(version) {
    }

    void ShardingConnectionHook::onCreate( DBClientBase * conn ) {

        // Authenticate as the first thing we do
        // NOTE: Replica set authentication allows authentication against *any* online host
        if(getGlobalAuthorizationManager()->isAuthEnabled()) {
            LOG(2) << "calling onCreate auth for " << conn->toString() << endl;

            bool result = authenticateInternalUser(conn);

            uassert( 15847, str::stream() << "can't authenticate to server "
                                          << conn->getServerAddress(), 
                     result );
        }

        // Initialize the wire version of single connections
        if (conn->type() == ConnectionString::MASTER) {

            LOG(2) << "checking wire version of new connection " << conn->toString();

            // Initialize the wire protocol version of the connection to find out if we
            // can send write commands to this connection.
            string errMsg;
            if (!initWireVersion(conn, &errMsg)) {
                uasserted(17363, errMsg);
            }
        }

        if ( _shardedConnections ) {
            // For every DBClient created by mongos, add a hook that will capture the response from
            // commands we pass along from the client, so that we can target the correct node when
            // subsequent getLastError calls are made by mongos.
            conn->setPostRunCommandHook(stdx::bind(&saveGLEStats, stdx::placeholders::_1, stdx::placeholders::_2));
        }

        // For every DBClient created by mongos, add a hook that will append impersonated users
        // to the end of every runCommand.  mongod uses this information to produce auditing
        // records attributed to the proper authenticated user(s).
        conn->setRunCommandHook(stdx::bind(&audit::appendImpersonatedUsers, stdx::placeholders::_1));

        // For every SCC created, add a hook that will allow fastest-config-first config reads if
        // the appropriate server options are set.
        if ( conn->type() == ConnectionString::SYNC ) {
            SyncClusterConnection* scc = dynamic_cast<SyncClusterConnection*>( conn );
            if ( scc ) {
                scc->attachQueryHandler( new SCCFastQueryHandler );
            }
        }
    }

    void ShardingConnectionHook::onDestroy( DBClientBase * conn ) {

        if( _shardedConnections && versionManager.isVersionableCB( conn ) ){
            versionManager.resetShardVersionCB( conn );
        }

    }

    void ShardingConnectionHook::onRelease(DBClientBase* conn) {
        // This is currently for making the replica set connections release
        // secondary connections to the pool.
        conn->reset();
    }
}
