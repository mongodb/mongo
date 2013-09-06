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

#include "mongo/pch.h"

#include "mongo/s/shard.h"

#include <set>
#include <string>
#include <vector>

#include "mongo/client/dbclient_rs.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/request.h"
#include "mongo/s/type_shard.h"
#include "mongo/s/version_manager.h"

namespace mongo {

    class StaticShardInfo {
    public:
        StaticShardInfo() : _mutex("StaticShardInfo"), _rsMutex("RSNameMap") { }
        void reload() {

            list<BSONObj> all;
            {
                ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);
                auto_ptr<DBClientCursor> c = conn->query(ShardType::ConfigNS , Query());
                massert( 13632 , "couldn't get updated shard list from config server" , c.get() );
                while ( c->more() ) {
                    all.push_back( c->next().getOwned() );
                }
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
            
            for ( list<BSONObj>::iterator i=all.begin(); i!=all.end(); ++i ) {
                BSONObj o = *i;
                string name = o[ ShardType::name() ].String();
                string host = o[ ShardType::host() ].String();

                long long maxSize = 0;
                BSONElement maxSizeElem = o[ ShardType::maxSize.name() ];
                if ( ! maxSizeElem.eoo() ) {
                    maxSize = maxSizeElem.numberLong();
                }

                bool isDraining = false;
                BSONElement isDrainingElem = o[ ShardType::draining.name() ];
                if ( ! isDrainingElem.eoo() ) {
                    isDraining = isDrainingElem.Bool();
                }

                ShardPtr s( new Shard( name , host , maxSize , isDraining ) );

                if ( o[ ShardType::tags() ].type() == Array ) {
                    vector<BSONElement> v = o[ ShardType::tags() ].Array();
                    for ( unsigned j=0; j<v.size(); j++ ) {
                        s->addTag( v[j].String() );
                    }
                }

                _lookup[name] = s;
                _installHost( host , s );
            }

        }

        ShardPtr find( const string& ident ) {
            string mykey = ident;

            {
                scoped_lock lk( _mutex );
                ShardMap::iterator i = _lookup.find( mykey );

                if ( i != _lookup.end() )
                    return i->second;
            }

            // not in our maps, re-load all
            reload();

            scoped_lock lk( _mutex );
            ShardMap::iterator i = _lookup.find( mykey );
            massert( 13129 , (string)"can't find shard for: " + mykey , i != _lookup.end() );
            return i->second;
        }

        // Lookup shard by replica set name. Returns Shard::EMTPY if the name can't be found.
        // Note: this doesn't refresh the table if the name isn't found, so it's possible that
        // a newly added shard/Replica Set may not be found.
        Shard lookupRSName( const string& name) {
            scoped_lock lk( _rsMutex );
            ShardMap::iterator i = _rsLookup.find( name );

            return (i == _rsLookup.end()) ? Shard::EMPTY : i->second.get();
        }

        // Useful for ensuring our shard data will not be modified while we use it
        Shard findCopy( const string& ident ){
            ShardPtr found = find( ident );
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
        ShardMap _lookup;
        ShardMap _rsLookup; // Map from ReplSet name to shard
        mutable mongo::mutex _mutex;
        mutable mongo::mutex _rsMutex;
    } staticShardInfo;


    class CmdGetShardMap : public Command {
    public:
        CmdGetShardMap() : Command( "getShardMap" ){}
        virtual void help( stringstream &help ) const { help<<"internal"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getShardMap);
            out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
        }
        virtual bool run(const string&, mongo::BSONObj&, int, std::string& errmsg , mongo::BSONObjBuilder& result, bool) {
            return staticShardInfo.getShardMap( result , errmsg );
        }
    } cmdGetShardMap;


    void Shard::_setAddr( const string& addr ) {
        _addr = addr;
        if ( !_addr.empty() ) {
            _cs = ConnectionString( addr , ConnectionString::SET );
        }
    }

    void Shard::setAddress( const ConnectionString& cs) {
        verify( _name.size() );
        _addr = cs.toString();
        _cs = cs;
        staticShardInfo.set( _name , *this , true , false );
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

            return rs->contains( node );
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
        return ShardStatus( *this , runCommand( "admin" , BSON( "serverStatus" << 1 ) ) );
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

    ShardStatus::ShardStatus( const Shard& shard , const BSONObj& obj )
        : _shard( shard ) {
        _mapped = obj.getFieldDotted( "mem.mapped" ).numberLong();
        _hasOpsQueued = obj["writeBacksQueued"].Bool();
        _writeLock = 0; // TODO
        _mongoVersion = obj["version"].String();
    }

    void ShardingConnectionHook::onCreate( DBClientBase * conn ) {
        if(AuthorizationManager::isAuthEnabled()) {
            LOG(2) << "calling onCreate auth for " << conn->toString() << endl;

            bool result = authenticateInternalUser(conn);

            uassert( 15847, str::stream() << "can't authenticate to server "
                                          << conn->getServerAddress(), 
                     result );
        }

        if ( _shardedConnections && versionManager.isVersionableCB( conn ) ) {

            // We must initialize sharding on all connections, so that we get exceptions if sharding is enabled on
            // the collection.
            BSONObj result;
            bool ok = versionManager.initShardVersionCB( conn, result );

            // assert that we actually successfully setup sharding
            uassert( 15907, str::stream() << "could not initialize sharding on connection " << (*conn).toString() <<
                        ( result["errmsg"].type() == String ? causedBy( result["errmsg"].String() ) :
                                                              causedBy( (string)"unknown failure : " + result.toString() ) ), ok );

        }
    }

    void ShardingConnectionHook::onDestroy( DBClientBase * conn ) {

        if( _shardedConnections && versionManager.isVersionableCB( conn ) ){
            versionManager.resetShardVersionCB( conn );
        }

    }
}
