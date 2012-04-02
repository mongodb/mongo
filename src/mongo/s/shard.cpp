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
 */

#include "pch.h"
#include "shard.h"
#include "config.h"
#include "request.h"
#include "client.h"
#include "../db/commands.h"
#include "mongo/client/dbclientcursor.h"
#include <set>

namespace mongo {

    typedef shared_ptr<Shard> ShardPtr;
    
    class StaticShardInfo {
    public:
        StaticShardInfo() : _mutex("StaticShardInfo"), _rsMutex("RSNameMap") { }
        void reload() {

            list<BSONObj> all;
            {
                ScopedDbConnection conn( configServer.getPrimary() );
                auto_ptr<DBClientCursor> c = conn->query( ShardNS::shard , Query() );
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
                string name = o["_id"].String();
                string host = o["host"].String();

                long long maxSize = 0;
                BSONElement maxSizeElem = o[ ShardFields::maxSize.name() ];
                if ( ! maxSizeElem.eoo() ) {
                    maxSize = maxSizeElem.numberLong();
                }

                bool isDraining = false;
                BSONElement isDrainingElem = o[ ShardFields::draining.name() ];
                if ( ! isDrainingElem.eoo() ) {
                    isDraining = isDrainingElem.Bool();
                }

                ShardPtr s( new Shard( name , host , maxSize , isDraining ) );
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

        virtual bool run(const string&, mongo::BSONObj&, int, std::string& errmsg , mongo::BSONObjBuilder& result, bool) {
            return staticShardInfo.getShardMap( result , errmsg );
        }
    } cmdGetShardMap;
    

    void Shard::_setAddr( const string& addr ) {
        _addr = addr;
        if ( !_addr.empty() ) {
            _cs = ConnectionString( addr , ConnectionString::SET );
            _rsInit();
        }
    }

    void Shard::_rsInit() {
        if ( _cs.type() == ConnectionString::SET ) {
            string x = _cs.getSetName();
            massert( 14807 , str::stream() << "no set name for shard: " << _name << " " << _cs.toString() , x.size() );
            _rs = ReplicaSetMonitor::get( x , _cs.getServers() );
        }
    }

    void Shard::setAddress( const ConnectionString& cs) {
        verify( _name.size() );
        _addr = cs.toString();
        _cs = cs;
        _rsInit();
        staticShardInfo.set( _name , *this , true , false );
    }

    void Shard::reset( const string& ident ) {
        *this = staticShardInfo.findCopy( ident );
        _rs.reset();
        _rsInit();
    }

    bool Shard::containsNode( const string& node ) const {
        if ( _addr == node )
            return true;
        
        if ( _rs && _rs->contains( node ) )
            return true;

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
        ScopedDbConnection conn( this );
        BSONObj res;
        bool ok = conn->runCommand( db , cmd , res );
        if ( ! ok ) {
            stringstream ss;
            ss << "runCommand (" << cmd << ") on shard (" << _name << ") failed : " << res;
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
    }

    void ShardingConnectionHook::onCreate( DBClientBase * conn ) {
        if( !noauth ) {
            string err;
            LOG(2) << "calling onCreate auth for " << conn->toString() << endl;
            uassert( 15847, "can't authenticate to shard server",
                    conn->auth("local", internalSecurity.user, internalSecurity.pwd, err, false));
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
