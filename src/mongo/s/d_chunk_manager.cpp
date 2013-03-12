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

#include "mongo/s/d_chunk_manager.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientmockcursor.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/instance.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"

namespace mongo {

    /**
     * This is an adapter so we can use config diffs - mongos and mongod do them slightly
     * differently
     *
     * The mongod adapter here tracks only a single shard, and stores ranges by (min, max)
     */
    class SCMConfigDiffTracker : public ConfigDiffTracker<BSONObj,string> {
    public:
        SCMConfigDiffTracker( const string& currShard ) : _currShard( currShard ) {}

        virtual bool isTracked( const BSONObj& chunkDoc ) const {
            return chunkDoc[ChunkType::shard()].type() == String &&
                   chunkDoc[ChunkType::shard()].String() == _currShard;
        }

        virtual BSONObj maxFrom( const BSONObj& val ) const {
            return val;
        }

        virtual pair<BSONObj,BSONObj> rangeFor( const BSONObj& chunkDoc, const BSONObj& min, const BSONObj& max ) const {
            return make_pair( min, max );
        }

        virtual string shardFor( const string& name ) const {
            return name;
        }

        virtual string nameFrom( const string& shard ) const {
            return shard;
        }

        string _currShard;

    };
    
    ShardChunkManager* ShardChunkManager::make( const string& configServer , const string& ns , const string& shardName, ShardChunkManagerPtr oldManager ) {
        auto_ptr<ShardChunkManager> m( new ShardChunkManager() );
        m->_init( configServer , ns , shardName , oldManager );
        return m.release();
    }

    void ShardChunkManager::_init( const string& configServer , const string& ns , const string& shardName, ShardChunkManagerPtr oldManager ) {

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
            scoped.reset(new ScopedDbConnection(configServer, 30.0));
            conn = scoped->get();
        }

        // get this collection's sharding key
        BSONObj collectionDoc = conn->findOne(CollectionType::ConfigNS, BSON(CollectionType::ns(ns)));

        if( collectionDoc.isEmpty() ){
            warning() << ns << " does not exist as a sharded collection" << endl;
            return;
        }

        if( collectionDoc[CollectionType::dropped()].Bool() ){
            warning() << ns << " was dropped.  Re-shard collection first." << endl;
            return;
        }

        _fillCollectionKey( collectionDoc );

        map<string,ChunkVersion> versionMap;
        versionMap[ shardName ] = _version;
        _collVersion = ChunkVersion( 0, OID() );

        // Check to see if we have an old ShardChunkManager to use
        if( oldManager && oldManager->_collVersion.isSet() ){

            versionMap[ shardName ] = oldManager->_version;
            _collVersion = oldManager->_collVersion;
            // TODO: This could be made more efficient if copying not required, but not as
            // frequently reloaded as in mongos.
            _chunksMap = oldManager->_chunksMap;

            LOG(2) << "loading new chunks for collection " << ns << " using old chunk manager w/ version " << _collVersion
                   << " and " << _chunksMap.size() << " chunks" << endl;
        }

        // Attach our config diff tracker to our range map and versions
        SCMConfigDiffTracker differ( shardName );
        differ.attach( ns, _chunksMap, _collVersion, versionMap );

        // Need to do the query ourselves, since we may use direct conns to the db
        Query query = differ.configDiffQuery();
        auto_ptr<DBClientCursor> cursor = conn->query(ChunkType::ConfigNS, query);

        uassert( 16181, str::stream() << "could not initialize cursor to config server chunks collection for ns " << ns, cursor.get() );

        // Diff tracker should *always* find at least one chunk if collection exists
        int diffsApplied = differ.calculateConfigDiff( *cursor );
        if( diffsApplied > 0 ){

            LOG(2) << "loaded " << diffsApplied << " chunks into new chunk manager for " << ns
                   << " with version " << _collVersion << endl;

            // Save the new version of this shard
            _version = versionMap[ shardName ];
            _fillRanges();

        }
        else if( diffsApplied == 0 ){

            // No chunks were found for the ns
            warning() << "no chunks found when reloading " << ns << ", previous version was " << _collVersion << endl;

            _version = ChunkVersion( 0, OID() );
            _collVersion = ChunkVersion( 0, OID() );
            _chunksMap.clear();
        }
        else{

            // TODO: make this impossible by making sure we don't migrate / split on this shard during the
            // reload
            // No chunks were found for the ns
            warning() << "invalid chunks found when reloading " << ns << ", previous version was " << _collVersion
                      << ", this should be rare" << endl;

            // Handle the same way as a connectivity error, for now
            // TODO: handle inline
            uassert( 16229,
                     str::stream() << "could not initialize cursor to config server chunks collection for ns "
                                   << ns, cursor.get() );
        }

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

        _key = e.Obj().getOwned();
    }

    void ShardChunkManager::_fillChunks( DBClientCursorInterface* cursor ) {
        verify( cursor );

        ChunkVersion version;
        while ( cursor->more() ) {
            BSONObj d = cursor->next();
            _chunksMap.insert(make_pair(d[ChunkType::min()].Obj().getOwned(),
                                        d[ChunkType::max()].Obj().getOwned()));

            ChunkVersion currVersion = ChunkVersion::fromBSON(d[ChunkType::DEPRECATED_lastmod()]);
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
        
        KeyPattern pat( _key );
        return _belongsToMe( cc->extractKey( pat ) );
    }

    bool ShardChunkManager::belongsToMe( const BSONObj& doc ) const {
        if ( _rangesMap.size() == 0 )
            return false;

        KeyPattern pat( _key );
        return _belongsToMe( pat.extractSingleKey( doc ) );
    }

    bool ShardChunkManager::_belongsToMe( const BSONObj& point ) const {
        RangeMap::const_iterator it = _rangesMap.upper_bound( point );
        if ( it != _rangesMap.begin() )
            it--;

        bool good = contains( it->first , it->second , point );

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

    ShardChunkManager* ShardChunkManager::cloneMinus( const BSONObj& min, const BSONObj& max, const ChunkVersion& version ) {

        // check that we have the exact chunk that will be subtracted
        _assertChunkExists( min , max );

        auto_ptr<ShardChunkManager> p( new ShardChunkManager );
        p->_key = this->_key;

        if ( _chunksMap.size() == 1 ) {
            // if left with no chunks, just reset version
            uassert( 13590 , str::stream() << "setting version to " << version.toString() << " on removing last chunk", ! version.isSet() );

            p->_version = ChunkVersion( 0, OID() );
            p->_collVersion = _collVersion;

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
            if( version > _collVersion ) p->_collVersion = version;
            else p->_collVersion = this->_collVersion;
            p->_fillRanges();
        }

        return p.release();
    }

    static bool overlap( const BSONObj& l1 , const BSONObj& h1 , const BSONObj& l2 , const BSONObj& h2 ) {
        return ! ( ( h1.woCompare( l2 ) <= 0 ) || ( h2.woCompare( l1 ) <= 0 ) );
    }

    ShardChunkManager* ShardChunkManager::clonePlus( const BSONObj& min , const BSONObj& max , const ChunkVersion& version ) {

        // it is acceptable to move version backwards (e.g., undoing a migration that went bad during commit)
        // but only cloning away the last chunk may reset the version to 0
        uassert( 13591 , "version can't be set to zero" , version.isSet() );

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
        if( version > _collVersion ) p->_collVersion = version;
        else p->_collVersion = this->_collVersion;
        p->_fillRanges();

        return p.release();
    }

    ShardChunkManager* ShardChunkManager::cloneSplit( const BSONObj& min , const BSONObj& max , const vector<BSONObj>& splitKeys ,
            const ChunkVersion& version ) {

        // the version required in both resulting chunks could be simply an increment in the minor portion of the current version
        // however, we are enforcing uniqueness over the attributes <ns, lastmod> of the configdb collection 'chunks'
        // so in practice, a migrate somewhere may force this split to pick up a version that has the major portion higher
        // than the one that this shard has been using
        //
        // TODO drop the uniqueness constraint and tighten the check below so that only the minor portion of version changes
        if ( version <= _version ) {
            uasserted( 14039 , str::stream() << "version " << version.toString() << " not greater than " << _version.toString() );
        }

        // check that we have the exact chunk that will be split and that the split point is valid
        _assertChunkExists( min , max );
        for ( vector<BSONObj>::const_iterator it = splitKeys.begin() ; it != splitKeys.end() ; ++it ) {
            if ( ! contains( min , max , *it ) ) {
                uasserted(14040, str::stream() << "cannot split " << min << " -> " << max
                        << " on " << *it);
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

        if( version > _collVersion ) p->_collVersion = version;
        else p->_collVersion = this->_collVersion;

        p->_fillRanges();

        return p.release();
    }

    string ShardChunkManager::toString() const {
        StringBuilder ss;
        ss << " ShardChunkManager version: " << _version.toString() << " key: " << _key;
        bool first = true;
        for ( RangeMap::const_iterator i=_rangesMap.begin(); i!=_rangesMap.end(); ++i ) {
            if ( first ) first = false;
            else ss << " , ";

            ss << i->first << " -> " << i->second;
        }
        return ss.str();
    }
    
}  // namespace mongo
