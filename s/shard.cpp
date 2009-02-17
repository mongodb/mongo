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

#include "stdafx.h"
#include "shard.h"
#include "config.h"

namespace mongo {
    
    Shard::Shard( ShardInfo * info , BSONObj data ) : _info( info ) , _data( data ){
    }

    string ShardInfo::modelServer() {
        // TODO: this could move around?
        return configServer.modelServer();
    }

    void ShardInfo::serialize(BSONObjBuilder& to) {
        to.append( "ns", _ns );
        to.append( "key" , _key.key() );
        
        BSONObjBuilder shards;
        int num=0;
        for ( vector<Shard>::iterator i=_shards.begin(); i != _shards.end(); i++  ){
            string s = shards.numStr( num++ );
            shards.append( s.c_str() , i->_data );
        }
        to.append( "shards" , shards.obj() );
    }

    void ShardInfo::unserialize(BSONObj& from) {
        _ns = from.getStringField("ns");
        uassert("bad config.shards.name", !_ns.empty());
        
        _key.init( from.getObjectField( "key" ) );

        _shards.clear();
        BSONObj shards = from.getObjectField( "shards" );
        if ( shards.isEmpty() ){
            BSONObjBuilder all;

            // TODO: server
            all.append( "min" , _key.globalMin() );
            all.append( "max" , _key.globalMax() );
            
            _shards.push_back( Shard( this , all.obj() ) );
        }
        else {
            int num=0;
            while ( true ){
                string s = BSONObjBuilder::numStr( num++ );            
                BSONObj next = shards.getObjectField( s.c_str() );
                if ( next.isEmpty() )
                    break;
                _shards.push_back( Shard( this , next ) );
            }
        }
    }
    
    void shardObjTest(){
        string ns = "alleyinsider.blog.posts";
        BSONObj o = BSON( "ns" << ns << "key" << BSON( "num" << 1 ) );

        ShardInfo si;
        si.unserialize( o );
        assert( si.getns() == ns );
        
        BSONObjBuilder b;
        si.serialize( b );
        BSONObj a = b.obj();
        assert( ns == a.getStringField( "ns" ) );
        assert( 1 == a.getObjectField( "key" )["num"].number() );

        log(2) << a << endl;
        
        {
            ShardInfo si2;
            si2.unserialize( a );
            BSONObjBuilder b2;
            si2.serialize( b2 );
            assert( b2.obj().jsonString() == a.jsonString() );
        }

        log(1) << "shardObjTest passed" << endl;
    }

} // namespace mongo
