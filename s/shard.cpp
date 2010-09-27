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
#include <set>

namespace mongo {
    
    class StaticShardInfo {
    public:
        StaticShardInfo() : _mutex("StaticShardInfo") { }
        void reload(){

            list<BSONObj> all;
            {
                ScopedDbConnection conn( configServer.getPrimary() );
                auto_ptr<DBClientCursor> c = conn->query( ShardNS::shard , Query() );
                assert( c.get() );
                while ( c->more() ){
                    all.push_back( c->next().getOwned() );
                }
                conn.done();
            }
            
            scoped_lock lk( _mutex );
            
            // We use the _lookup table for all shards and for the primary config DB. The config DB info,
            // however, does not come from the ShardNS::shard. So when cleaning the _lookup table we leave
            // the config state intact. The rationale is that this way we could drop shards that
            // were removed without reinitializing the config DB information.

            map<string,Shard>::iterator i = _lookup.find( "config" );
            if ( i != _lookup.end() ){
                Shard config = i->second;
                _lookup.clear();
                _lookup[ "config" ] = config;
            } else {
                _lookup.clear();
            }

            for ( list<BSONObj>::iterator i=all.begin(); i!=all.end(); ++i ){
                BSONObj o = *i;
                string name = o["_id"].String();
                string host = o["host"].String();

                long long maxSize = 0;
                BSONElement maxSizeElem = o[ ShardFields::maxSize.name() ];
                if ( ! maxSizeElem.eoo() ){
                    maxSize = maxSizeElem.numberLong();
                }

                bool isDraining = false;
                BSONElement isDrainingElem = o[ ShardFields::draining.name() ];
                if ( ! isDrainingElem.eoo() ){
                    isDraining = isDrainingElem.Bool();
                }

                Shard s( name , host , maxSize , isDraining );
                _lookup[name] = s;
                _lookup[host] = s;

                // add rs name to lookup (if it exists)
                size_t pos;
                if ((pos = host.find('/', 0)) != string::npos) {
                    _lookup[host.substr(0, pos)] = s;
                }
            }

        }
        
        bool isMember( const string& addr ){
            scoped_lock lk( _mutex );
            map<string,Shard>::iterator i = _lookup.find( addr );
            return i != _lookup.end();
        }

        const Shard& find( const string& ident ){
            {
                scoped_lock lk( _mutex );
                map<string,Shard>::iterator i = _lookup.find( ident );

                // if normal find didn't find anything, try to find by rs name
                size_t pos;
                if ( i == _lookup.end() && (pos = ident.find('/', 0)) != string::npos) {
                    i = _lookup.find( ident.substr(0, pos) );
                }

                if ( i != _lookup.end() )
                    return i->second;
            }
            
            // not in our maps, re-load all
            reload();

            scoped_lock lk( _mutex );
            map<string,Shard>::iterator i = _lookup.find( ident );
            uassert( 13129 , (string)"can't find shard for: " + ident , i != _lookup.end() );
            return i->second;        
        }
        
        void set( const string& name , const string& addr , bool setName = true , bool setAddr = true ){
            Shard s(name,addr);
            scoped_lock lk( _mutex );
            if ( setName )
                _lookup[name] = s;
            if ( setAddr )
                _lookup[addr] = s;
        }
        
        void remove( const string& name ){
            scoped_lock lk( _mutex );
            for ( map<string,Shard>::iterator i = _lookup.begin(); i!=_lookup.end(); ){
                Shard s = i->second;
                if ( s.getName() == name ){
                    _lookup.erase(i++);
                } else {
                    ++i;
                }
            }
        }

        void getAllShards( vector<Shard>& all ){
            scoped_lock lk( _mutex );
            std::set<string> seen;
            for ( map<string,Shard>::iterator i = _lookup.begin(); i!=_lookup.end(); ++i ){
                Shard s = i->second;
                if ( s.getName() == "config" )
                    continue;
                if ( seen.count( s.getName() ) )
                    continue;
                seen.insert( s.getName() );
                all.push_back( s );
            }
        }

    private:
        map<string,Shard> _lookup;
        mongo::mutex _mutex;        
    } staticShardInfo;
    
    void Shard::setAddress( const string& addr , bool authoritative ){
        assert( _name.size() );
        _addr = addr;
        if ( authoritative )
            staticShardInfo.set( _name , _addr , true , false );
    }
    
    void Shard::reset( const string& ident ){
        const Shard& s = staticShardInfo.find( ident );
        uassert( 13128 , (string)"can't find shard for: " + ident , s.ok() );
        _name = s._name;
        _addr = s._addr;
        _maxSize = s._maxSize;
        _isDraining = s._isDraining;
    }
    
    void Shard::getAllShards( vector<Shard>& all ){
        staticShardInfo.getAllShards( all );
    }

    bool Shard::isAShard( const string& ident ){
        return staticShardInfo.isMember( ident );
    }

    void Shard::printShardInfo( ostream& out ){
        vector<Shard> all;
        getAllShards( all );
        for ( unsigned i=0; i<all.size(); i++ )
            out << all[i].toString() << "\n";
        out.flush();
    }
    
    BSONObj Shard::runCommand( const string& db , const BSONObj& cmd ) const {
        ScopedDbConnection conn( this );
        BSONObj res;
        bool ok = conn->runCommand( db , cmd , res );
        if ( ! ok ){
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
    
    void Shard::reloadShardInfo(){
        staticShardInfo.reload();
    }


    bool Shard::isMember( const string& addr ){
        return staticShardInfo.isMember( addr );
    }
  
    void Shard::removeShard( const string& name ){
        staticShardInfo.remove( name );
    }

    Shard Shard::pick( const Shard& current ){
        vector<Shard> all;
        staticShardInfo.getAllShards( all );
        if ( all.size() == 0 ){
            staticShardInfo.reload();
            staticShardInfo.getAllShards( all );
            if ( all.size() == 0 )
                return EMPTY;
        }
        
        // if current shard was provided, pick a different shard only if it is a better choice
        ShardStatus best = all[0].getStatus();
        if ( current != EMPTY ){
            best = current.getStatus();
        }
            
        for ( size_t i=0; i<all.size(); i++ ){
            ShardStatus t = all[i].getStatus();
            if ( t < best )
                best = t;
        }

        log(1) << "best shard for new allocation is " << best << endl;
        return best.shard();
    }

    ShardStatus::ShardStatus( const Shard& shard , const BSONObj& obj )
        : _shard( shard ) {
        _mapped = obj.getFieldDotted( "mem.mapped" ).numberLong();
        _hasOpsQueued = obj["writeBacksQueued"].Bool();
        _writeLock = 0; // TODO
    }

}
