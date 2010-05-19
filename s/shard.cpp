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
        
        void reload(){

            list<BSONObj> all;
            {
                ShardConnection conn( configServer.getPrimary() );
                auto_ptr<DBClientCursor> c = conn->query( ShardNS::shard , Query() );
                while ( c->more() ){
                    all.push_back( c->next().getOwned() );
                }
                conn.done();
            }
            
            scoped_lock lk( _mutex );
            
            for ( list<BSONObj>::iterator i=all.begin(); i!=all.end(); ++i ){
                BSONObj o = *i;
                string name = o["_id"].String();
                string host = o["host"].String();
                Shard s( name , host );
                _lookup[name] = s;
                _lookup[host] = s;
            }

        }
        
        const Shard& find( const string& ident ){
            {
                scoped_lock lk( _mutex );
                map<string,Shard>::iterator i = _lookup.find( ident );
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
    }
    
    void Shard::getAllShards( vector<Shard>& all ){
        staticShardInfo.getAllShards( all );
    }

    
    BSONObj Shard::runCommand( const string& db , const BSONObj& cmd ) const {
        ShardConnection conn( this );
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
    
    Shard Shard::pick(){
        vector<Shard> all;
        staticShardInfo.getAllShards( all );
        if ( all.size() == 0 ){
            staticShardInfo.reload();
            staticShardInfo.getAllShards( all );
            if ( all.size() == 0 )
                return EMPTY;
        }
        
        ShardStatus best = all[0].getStatus();
        
        for ( size_t i=1; i<all.size(); i++ ){
            ShardStatus t = all[i].getStatus();
            if ( t < best )
                best = t;
        }

        log(1) << "picking shard: " << best << endl;
        return best.shard();
    }

    ShardStatus::ShardStatus( const Shard& shard , const BSONObj& obj )
        : _shard( shard ) {
        _mapped = obj.getFieldDotted( "mem.mapped" ).numberLong();
        _writeLock = 0; // TOOD
    }


}
