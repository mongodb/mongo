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

#include "../stdafx.h"
#include "shard.h"
#include "config.h"

namespace mongo {
    
    class StaticShardInfo {
    public:
        
        const Shard& find( const string& ident ){
            {
                scoped_lock lk( _mutex );
                map<string,Shard>::iterator i = _lookup.find( ident );
                if ( i != _lookup.end() )
                    return i->second;
            }
            
            // not in our maps, re-load all
            
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
            
            map<string,Shard>::iterator i = _lookup.find( ident );
            if ( i == _lookup.end() ) printStackTrace();
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
}
