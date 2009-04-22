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

namespace mongo {

    class DBConfig;
    class ShardManager;
    class ShardObjUnitTest;

    typedef unsigned long long ServerShardVersion;

    /**
       config.shard
       { ns : "alleyinsider.fs.chunks" , min : {} , max : {} , server : "localhost:30001" }
     */    
    class Shard : public Model , boost::noncopyable {
    public:

        Shard( ShardManager * info );
        
        BSONObj& getMin(){
            return _min;
        }
        BSONObj& getMax(){
            return _max;
        }

        string getServer(){
            return _server;
        }
        void setServer( string server );

        bool contains( const BSONObj& obj );

        string toString() const;
        operator string() const { return toString(); }

        bool operator==(const Shard& s);
        
        bool operator!=(const Shard& s){
            return ! ( *this == s );
        }
        
        void getFilter( BSONObjBuilder& b );

        BSONObj pickSplitPoint();
        Shard * split();
        Shard * split( const BSONObj& middle );

        /**
         * @return size of shard in bytes
         *  talks to mongod to do this
         */
        long getPhysicalSize();
        
        long countObjects();
        
        /**
         * if the amount of data written nears the max size of a shard
         * then we check the real size, and if its too big, we split
         */
        bool splitIfShould( long dataWritten );
        

        /*
         * moves either this shard or newShard if it makes sense too
         * @return whether or not a shard was moved
         */
        bool moveIfShould( Shard * newShard = 0 );

        bool moveAndCommit( const string& to , string& errmsg );

        virtual const char * getNS(){ return "config.shard"; }
        virtual void serialize(BSONObjBuilder& to);
        virtual void unserialize(const BSONObj& from);
        virtual string modelServer();

        virtual void save( bool check=false );
        
        void ensureIndex();
        
        void _markModified();
        
        static long MaxShardSize;

    private:
        
        // main shard info
        
        ShardManager * _manager;
        ShardKeyPattern skey();

        string _ns;
        BSONObj _min;
        BSONObj _max;
        string _server;
        ServerShardVersion _lastmod;

        bool _modified;
        
        // transient stuff

        long _dataWritten;

        // methods, etc..
        
        void _split( BSONObj& middle );

        friend class ShardManager;
        friend class ShardObjUnitTest;
    };

    /* config.sharding
         { ns: 'alleyinsider.fs.chunks' , 
           key: { ts : 1 } ,
           shards: [ { min: 1, max: 100, server: a } , { min: 101, max: 200 , server : b } ]
         }
    */
    class ShardManager {
    public:

        ShardManager( DBConfig * config , string ns ,ShardKeyPattern pattern );
        virtual ~ShardManager();

        string getns(){
            return _ns;
        }
        
        int numShards(){ return _shards.size(); }
        bool hasShardKey( const BSONObj& obj );

        Shard& findShard( const BSONObj& obj );
        Shard* findShardOnServer( const string& server ) const;
        
        ShardKeyPattern& getShardKey(){  return _key; }
        
        /**
         * makes sure the shard index is on all servers
         */
        void ensureIndex();

        /**
         * @return number of shards added to the vector
         */
        int getShardsForQuery( vector<Shard*>& shards , const BSONObj& query );

        void save();

        string toString() const;
        operator string() const { return toString(); }

        ServerShardVersion getVersion( const string& server ) const;
        ServerShardVersion getVersion() const;

        /**
         * this is just an increasing number of how many shard managers we have so we know if something has been updated
         */
        unsigned long long getSequenceNumber(){
            return _sequenceNumber;
        }
        
    private:
        DBConfig * _config;
        string _ns;
        ShardKeyPattern _key;
        
        vector<Shard*> _shards;
        
        unsigned long long _sequenceNumber;
        
        friend class Shard;
        static unsigned long long NextSequenceNumber;
    };

} // namespace mongo
