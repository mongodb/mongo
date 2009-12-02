// shard.h

/*
   A "shard" is a database (replica pair typically) which represents
   one partition of the overall database.
*/

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

#pragma once

#include "../stdafx.h"
#include "../client/dbclient.h"
#include "../client/model.h"
#include "shardkey.h"
#include <boost/utility.hpp>
#undef assert
#define assert xassert

namespace mongo {

    class DBConfig;
    class ChunkManager;
    class ChunkObjUnitTest;

    typedef unsigned long long ShardChunkVersion;
    
    /**
       config.chunks
       { ns : "alleyinsider.fs.chunks" , min : {} , max : {} , server : "localhost:30001" }
       
       x is in a shard iff
       min <= x < max
     */    
    class Chunk : public Model , boost::noncopyable {
    public:

        Chunk( ChunkManager * info );
        
        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMinDotted() const { return _minDotted; }
        const BSONObj& getMax() const { return _max; }
        const BSONObj& getMaxDotted() const { return _maxDotted; }
        
        void setMin(const BSONObj& o){
            _min = o;
            _minDotted = nested2dotted(o);
        }
        void setMax(const BSONObj& o){
            _max = o;
            _maxDotted = nested2dotted(o);
        }

        string getShard(){
            return _shard;
        }
        void setShard( string shard );

        bool contains( const BSONObj& obj );

        string toString() const;
        operator string() const { return toString(); }

        bool operator==(const Chunk& s);
        
        bool operator!=(const Chunk& s){
            return ! ( *this == s );
        }
        
        void getFilter( BSONObjBuilder& b );
        BSONObj getFilter(){ BSONObjBuilder b; getFilter( b ); return b.obj(); }
            

        BSONObj pickSplitPoint();
        Chunk * split();
        Chunk * split( const BSONObj& middle );

        /**
         * @return size of shard in bytes
         *  talks to mongod to do this
         */
        long getPhysicalSize();
        
        long countObjects( const BSONObj& filter = BSONObj() );
        
        /**
         * if the amount of data written nears the max size of a shard
         * then we check the real size, and if its too big, we split
         */
        bool splitIfShould( long dataWritten );
        

        /*
         * moves either this shard or newShard if it makes sense too
         * @return whether or not a shard was moved
         */
        bool moveIfShould( Chunk * newShard = 0 );

        bool moveAndCommit( const string& to , string& errmsg );

        virtual const char * getNS(){ return "config.chunks"; }
        virtual void serialize(BSONObjBuilder& to);
        virtual void unserialize(const BSONObj& from);
        virtual string modelServer();

        virtual void save( bool check=false );
        
        void ensureIndex();
        
        void _markModified();
        
        static long MaxChunkSize;

    private:
        
        // main shard info
        
        ChunkManager * _manager;
        ShardKeyPattern skey();

        string _ns;
        BSONObj _min; //nested
        BSONObj _minDotted;
        BSONObj _max; //nested
        BSONObj _maxDotted;
        string _shard;
        ShardChunkVersion _lastmod;

        bool _modified;
        
        // transient stuff

        long _dataWritten;

        // methods, etc..
        
        void _split( BSONObj& middle );

        friend class ChunkManager;
        friend class ShardObjUnitTest;
    };

    /* config.sharding
         { ns: 'alleyinsider.fs.chunks' , 
           key: { ts : 1 } ,
           shards: [ { min: 1, max: 100, server: a } , { min: 101, max: 200 , server : b } ]
         }
    */
    class ChunkManager {
    public:

        ChunkManager( DBConfig * config , string ns , ShardKeyPattern pattern , bool unique );
        virtual ~ChunkManager();

        string getns(){
            return _ns;
        }
        
        int numChunks(){ return _chunks.size(); }
        Chunk* getChunk( int i ){ return _chunks[i]; }
        bool hasShardKey( const BSONObj& obj );

        Chunk& findChunk( const BSONObj& obj );
        Chunk* findChunkOnServer( const string& server ) const;
        
        ShardKeyPattern& getShardKey(){  return _key; }
        bool isUnique(){ return _unique; }
        
        /**
         * makes sure the shard index is on all servers
         */
        void ensureIndex();

        /**
         * @return number of Chunk added to the vector
         */
        int getChunksForQuery( vector<Chunk*>& chunks , const BSONObj& query );

        void getAllServers( set<string>& allServers );

        void save();

        string toString() const;
        operator string() const { return toString(); }

        ShardChunkVersion getVersion( const string& server ) const;
        ShardChunkVersion getVersion() const;

        /**
         * this is just an increasing number of how many ChunkManagers we have so we know if something has been updated
         */
        unsigned long long getSequenceNumber(){
            return _sequenceNumber;
        }

        void drop();
        
    private:
        DBConfig * _config;
        string _ns;
        ShardKeyPattern _key;
        bool _unique;
        
        vector<Chunk*> _chunks;
        map<string,unsigned long long> _maxMarkers;

        unsigned long long _sequenceNumber;
        
        friend class Chunk;
        static unsigned long long NextSequenceNumber;
    };

} // namespace mongo
