// @file d_chunk_matcher.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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

#include "../client/connpool.h"
#include "../db/instance.h"

#include "d_chunk_matcher.h"

namespace mongo {

    ChunkMatcher::ChunkMatcher( const string& configServer , const string& ns , const string& shardName ) { 

        // have to get a connection to the config db
        // special case if i'm the configdb since i'm locked and if i connect to myself
        // its a deadlock
        auto_ptr<ScopedDbConnection> scoped;
        auto_ptr<DBDirectClient> direct;
        DBClientBase * conn;
        if ( configServer.empty() ){
            direct.reset( new DBDirectClient() );
            conn = direct.get();
        } else {
            scoped.reset( new ScopedDbConnection( configServer ) );
            conn = scoped->get();
        }

        // get this collection's key
        BSONObj collection = conn->findOne( "config.collections", BSON( "_id" << ns ) );
        assert( ! collection["key"].eoo() && collection["key"].isABSONObj() );
        BSONObj key = collection["key"].Obj().getOwned();
        BSONObjBuilder b;
        BSONForEach( e , key ) {
            b.append( e.fieldName() , 1 );
        }
        _key = b.obj();

        // actually query all the chunks for 'ns' that live in this shard, sorting so we can efficiently bucket them
        BSONObj q;
        {
            BSONObjBuilder b;
            b.append( "ns" , ns.c_str() );
            b.append( "shard" , shardName );
            q = b.obj();
        }
        auto_ptr<DBClientCursor> cursor = conn->query( "config.chunks" , Query(q).sort( "min" ) );

        assert( cursor.get() );
        if ( ! cursor->more() ){
            log() << "No chunks for collection " << ns << " on shard " << shardName << endl;
            if ( scoped.get() )
                scoped->done();

            return;
        }

        // load the tablet information, coallesceing the ranges
        // the version for this shard would be the highest version for any of the chunks
        ShardChunkVersion version;
        BSONObj min,max;
        while ( cursor->more() ){
            BSONObj d = cursor->next();

            _chunksMap[min] == make_pair( d["min"].Obj().getOwned() , d["max"].Obj().getOwned() );

            ShardChunkVersion currVersion( d["lastmod"] );
            if ( currVersion > version ) {
                version = currVersion;
            }
            
            // coallesce the chunk's bounds in ranges if they are adjacent chunks 
            if ( min.isEmpty() ){
                min = d["min"].Obj().getOwned();
                max = d["max"].Obj().getOwned();
                continue;
            }
            if ( max == d["min"].Obj() ){
                max = d["max"].Obj().getOwned();
                continue;
            }

            _rangesMap[min] = make_pair( min.getOwned() , max.getOwned() );

            min = d["min"].Obj().getOwned();
            max = d["max"].Obj().getOwned();
        }
        assert( ! min.isEmpty() );

        _rangesMap[min] = make_pair( min.getOwned() , max.getOwned() );
        _version = version;
        
        if ( scoped.get() )
            scoped->done();
    }

    bool ChunkMatcher::belongsToMe( const BSONObj& obj ) const {
        if ( _rangesMap.size() == 0 )
            return false;

        BSONObj x = obj.extractFields(_key);

        RangeMap::const_iterator a = _rangesMap.upper_bound( x );
        if ( a != _rangesMap.begin() )
            a--;
        
        bool good = x.woCompare( a->second.first ) >= 0 && x.woCompare( a->second.second ) < 0;
#if 0
        if ( ! good ){
            cout << "bad: " << x << "\t" << a->second.first << "\t" << x.woCompare( a->second.first ) << "\t" << x.woCompare( a->second.second ) << endl;
            for ( MyMap::const_iterator i=_map.begin(); i!=_map.end(); ++i ){
                cout << "\t" << i->first << "\t" << i->second.first << "\t" << i->second.second << endl;
            }
        }
#endif
        return good;
    }
    
}  // namespace mongo
