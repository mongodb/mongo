// @file d_chunk_manager.cpp

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
#include "../client/dbclientmockcursor.h"
#include "../db/instance.h"
#include "../db/clientcursor.h"

#include "d_chunk_manager.h"

namespace mongo {

    ShardChunkManager::ShardChunkManager( const string& configServer , const string& ns , const string& shardName ) {

        // have to get a connection to the config db
        // special case if I'm the configdb since I'm locked and if I connect to myself
        // its a deadlock
        scoped_ptr<ScopedDbConnection> scoped;
        scoped_ptr<DBDirectClient> direct;
        DBClientBase * conn;
        if ( configServer.empty() ) {
            direct.reset( new DBDirectClient() );
            conn = direct.get();
        }
        else {
            scoped.reset( new ScopedDbConnection( configServer, 30.0 ) );
            conn = scoped->get();
        }

        // get this collection's sharding key
        BSONObj collectionDoc = conn->findOne( "config.collections", BSON( "_id" << ns ) );
        uassert( 13539 , str::stream() << ns << " does not exist" , !collectionDoc.isEmpty() );
        uassert( 13540 , str::stream() << ns << " collection config entry corrupted" , collectionDoc["dropped"].type() );
        uassert( 13541 , str::stream() << ns << " dropped. Re-shard collection first." , !collectionDoc["dropped"].Bool() );
        _fillCollectionKey( collectionDoc );

        // query for all the chunks for 'ns' that live in this shard, sorting so we can efficiently bucket them
        BSONObj q = BSON( "ns" << ns << "shard" << shardName );
        auto_ptr<DBClientCursor> cursor = conn->query( "config.chunks" , Query(q).sort( "min" ) );
        _fillChunks( cursor.get() );
        _fillRanges();

        if ( scoped.get() )
            scoped->done();

        if ( _chunksMap.empty() )
            log() << "no chunk for collection " << ns << " on shard " << shardName << endl;
    }

    ShardChunkManager::ShardChunkManager( const BSONObj& collectionDoc , const BSONArray& chunksArr ) {
        _fillCollectionKey( collectionDoc );

        scoped_ptr<DBClientMockCursor> c ( new DBClientMockCursor( chunksArr ) );
        _fillChunks( c.get() );
        _fillRanges();
    }

    void ShardChunkManager::_fillCollectionKey( const BSONObj& collectionDoc ) {
        BSONElement e = collectionDoc["key"];
        uassert( 13542 , str::stream() << "collection doesn't have a key: " << collectionDoc , ! e.eoo() && e.isABSONObj() );

        BSONObj keys = e.Obj().getOwned();
        BSONObjBuilder b;
        BSONForEach( key , keys ) {
            b.append( key.fieldName() , 1 );
        }
        _key = b.obj();
    }

    void ShardChunkManager::_fillChunks( DBClientCursorInterface* cursor ) {
        verify( cursor );

        ShardChunkVersion version;
        while ( cursor->more() ) {
            BSONObj d = cursor->next();
            _chunksMap.insert( make_pair( d["min"].Obj().getOwned() , d["max"].Obj().getOwned() ) );

            ShardChunkVersion currVersion( d["lastmod"] );
            if ( currVersion > version ) {
                version = currVersion;
            }
        }
        _version = version;
    }

    void ShardChunkManager::_fillRanges() {
        if ( _chunksMap.empty() )
            return;

        // load the chunk information, coallesceing their ranges
        // the version for this shard would be the highest version for any of the chunks
        RangeMap::const_iterator it = _chunksMap.begin();
        BSONObj min,max;
        while ( it != _chunksMap.end() ) {
            BSONObj currMin = it->first;
            BSONObj currMax = it->second;
            ++it;

            // coalesce the chunk's bounds in ranges if they are adjacent chunks
            if ( min.isEmpty() ) {
                min = currMin;
                max = currMax;
                continue;
            }
            if ( max == currMin ) {
                max = currMax;
                continue;
            }

            _rangesMap.insert( make_pair( min , max ) );

            min = currMin;
            max = currMax;
        }
        verify( ! min.isEmpty() );

        _rangesMap.insert( make_pair( min , max ) );
    }

    static bool contains( const BSONObj& min , const BSONObj& max , const BSONObj& point ) {
        return point.woCompare( min ) >= 0 && point.woCompare( max ) < 0;
    }
    
    bool ShardChunkManager::belongsToMe( ClientCursor* cc ) const {
        verify( cc );
        if ( _rangesMap.size() == 0 )
            return false;
        
        return _belongsToMe( cc->extractFields( _key , true ) );
    }

    bool ShardChunkManager::belongsToMe( const BSONObj& obj ) const {
        if ( _rangesMap.size() == 0 )
            return false;

        return _belongsToMe( obj.extractFields( _key , true ) );
    }

    bool ShardChunkManager::_belongsToMe( const BSONObj& x ) const {
        RangeMap::const_iterator it = _rangesMap.upper_bound( x );
        if ( it != _rangesMap.begin() )
            it--;

        bool good = contains( it->first , it->second , x );

#if 0
        if ( ! good ) {
            log() << "bad: " << x << " " << it->first << " " << x.woCompare( it->first ) << " " << x.woCompare( it->second ) << endl;
            for ( RangeMap::const_iterator i=_rangesMap.begin(); i!=_rangesMap.end(); ++i ) {
                log() << "\t" << i->first << "\t" << i->second << "\t" << endl;
            }
        }
#endif

        return good;
    }

    bool ShardChunkManager::getNextChunk( const BSONObj& lookupKey, BSONObj* foundMin , BSONObj* foundMax ) const {
        verify( foundMin );
        verify( foundMax );
        *foundMin = BSONObj();
        *foundMax = BSONObj();

        if ( _chunksMap.empty() ) {
            return true;
        }

        RangeMap::const_iterator it;
        if ( lookupKey.isEmpty() ) {
            it = _chunksMap.begin();
            *foundMin = it->first;
            *foundMax = it->second;
            return _chunksMap.size() == 1;
        }

        it = _chunksMap.upper_bound( lookupKey );
        if ( it != _chunksMap.end() ) {
            *foundMin = it->first;
            *foundMax = it->second;
            return false;
        }

        return true;
    }

    void ShardChunkManager::_assertChunkExists( const BSONObj& min , const BSONObj& max ) const {
        RangeMap::const_iterator it = _chunksMap.find( min );
        if ( it == _chunksMap.end() ) {
            uasserted( 13586 , str::stream() << "couldn't find chunk " << min << "->" << max );
        }

        if ( it->second.woCompare( max ) != 0 ) {
            ostringstream os;
            os << "ranges differ, "
               << "requested: "  << min << " -> " << max << " "
               << "existing: " << ((it == _chunksMap.end()) ? "<empty>" : it->first.toString() + " -> " + it->second.toString());
            uasserted( 13587 , os.str() );
        }
    }

    ShardChunkManager* ShardChunkManager::cloneMinus( const BSONObj& min, const BSONObj& max, const ShardChunkVersion& version ) {

        // check that we have the exact chunk that will be subtracted
        _assertChunkExists( min , max );

        auto_ptr<ShardChunkManager> p( new ShardChunkManager );
        p->_key = this->_key;

        if ( _chunksMap.size() == 1 ) {
            // if left with no chunks, just reset version
            uassert( 13590 , str::stream() << "setting version to " << version << " on removing last chunk", version == 0 );

            p->_version = 0;

        }
        else {
            // can't move version backwards when subtracting chunks
            // this is what guarantees that no read or write would be taken once we subtract data from the current shard
            if ( version <= _version ) {
                uasserted( 13585 , str::stream() << "version " << version.toString() << " not greater than " << _version.toString() );
            }

            p->_chunksMap = this->_chunksMap;
            p->_chunksMap.erase( min );
            p->_version = version;
            p->_fillRanges();
        }

        return p.release();
    }

    static bool overlap( const BSONObj& l1 , const BSONObj& h1 , const BSONObj& l2 , const BSONObj& h2 ) {
        return ! ( ( h1.woCompare( l2 ) <= 0 ) || ( h2.woCompare( l1 ) <= 0 ) );
    }

    ShardChunkManager* ShardChunkManager::clonePlus( const BSONObj& min , const BSONObj& max , const ShardChunkVersion& version ) {

        // it is acceptable to move version backwards (e.g., undoing a migration that went bad during commit)
        // but only cloning away the last chunk may reset the version to 0
        uassert( 13591 , "version can't be set to zero" , version > 0 );

        if ( ! _chunksMap.empty() ) {

            // check that there isn't any chunk on the interval to be added
            RangeMap::const_iterator it = _chunksMap.lower_bound( max );
            if ( it != _chunksMap.begin() ) {
                --it;
            }
            if ( overlap( min , max , it->first , it->second ) ) {
                ostringstream os;
                os << "ranges overlap, "
                   << "requested: " << min << " -> " << max << " "
                   << "existing: " << it->first.toString() + " -> " + it->second.toString();
                uasserted( 13588 , os.str() );
            }
        }

        auto_ptr<ShardChunkManager> p( new ShardChunkManager );

        p->_key = this->_key;
        p->_chunksMap = this->_chunksMap;
        p->_chunksMap.insert( make_pair( min.getOwned() , max.getOwned() ) );
        p->_version = version;
        p->_fillRanges();

        return p.release();
    }

    ShardChunkManager* ShardChunkManager::cloneSplit( const BSONObj& min , const BSONObj& max , const vector<BSONObj>& splitKeys ,
            const ShardChunkVersion& version ) {

        // the version required in both resulting chunks could be simply an increment in the minor portion of the current version
        // however, we are enforcing uniqueness over the attributes <ns, lastmod> of the configdb collection 'chunks'
        // so in practice, a migrate somewhere may force this split to pick up a version that has the major portion higher
        // than the one that this shard has been using
        //
        // TODO drop the uniqueness constraint and tigthen the check below so that only the minor portion of version changes
        if ( version <= _version ) {
            uasserted( 14039 , str::stream() << "version " << version.toString() << " not greater than " << _version.toString() );
        }

        // check that we have the exact chunk that will be split and that the split point is valid
        _assertChunkExists( min , max );
        for ( vector<BSONObj>::const_iterator it = splitKeys.begin() ; it != splitKeys.end() ; ++it ) {
            if ( ! contains( min , max , *it ) ) {
                uasserted( 14040 , str::stream() << "can split " << min << " -> " << max << " on " << *it );
            }
        }

        auto_ptr<ShardChunkManager> p( new ShardChunkManager );

        p->_key = this->_key;
        p->_chunksMap = this->_chunksMap;
        p->_version = version; // will increment second, third, ... chunks below

        BSONObj startKey = min;
        for ( vector<BSONObj>::const_iterator it = splitKeys.begin() ; it != splitKeys.end() ; ++it ) {
            BSONObj split = *it;
            p->_chunksMap[min] = split.getOwned();
            p->_chunksMap.insert( make_pair( split.getOwned() , max.getOwned() ) );
            p->_version.incMinor();
            startKey = split;
        }
        p->_fillRanges();

        return p.release();
    }

    string ShardChunkManager::toString() const {
        StringBuilder ss;
        ss << " ShardChunkManager version: " << _version << " key: " << _key;
        bool first = true;
        for ( RangeMap::const_iterator i=_rangesMap.begin(); i!=_rangesMap.end(); ++i ) {
            if ( first ) first = false;
            else ss << " , ";

            ss << i->first << " -> " << i->second;
        }
        return ss.str();
    }
    
}  // namespace mongo
