// @file d_logic.h
/*
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


#pragma once

#include "../pch.h"

#include "../db/jsobj.h"

#include "d_chunk_matcher.h"
#include "util.h"

namespace mongo {
    
    typedef ShardChunkVersion ConfigVersion;
    typedef map<string,ConfigVersion> NSVersionMap;

    // --------------
    // --- global state ---
    // --------------

    class ShardingState {
    public:
        ShardingState();
        
        bool enabled() const { return _enabled; }
        const string& getConfigServer() const { return _configServer; }
        void enable( const string& server );

        void gotShardName( const string& name );
        void gotShardHost( string host );
        
        bool hasVersion( const string& ns );
        bool hasVersion( const string& ns , ConfigVersion& version );
        const ConfigVersion getVersion( const string& ns ) const;
        void setVersion( const string& ns , const ConfigVersion& version );
        
        void appendInfo( BSONObjBuilder& b );
        
        bool needChunkMatcher( const string& ns ) const;
        ChunkMatcherPtr getChunkMatcher( const string& ns );
        
        bool inCriticalMigrateSection();
    private:
        
        bool _enabled;
        
        string _configServer;
        
        string _shardName;
        string _shardHost;

        // protects state below
        mutable mongo::mutex _mutex;

        // map from a namespace into the highest ShardChunkVersion for that collection
        NSVersionMap _versions;

        // map from a namespace into the ensemble of chunk ranges that are stored in this mongod
        map<string,ChunkMatcherPtr> _chunks;
    };
    
    extern ShardingState shardingState;

    /**
     * one per connection from mongos
     * holds version state for each namesapce
     */
    class ShardedConnectionInfo {
    public:
        ShardedConnectionInfo();
        
        const OID& getID() const { return _id; }
        bool hasID() const { return _id.isSet(); }
        void setID( const OID& id );
        
        const ConfigVersion getVersion( const string& ns ) const;
        void setVersion( const string& ns , const ConfigVersion& version );
        
        static ShardedConnectionInfo* get( bool create );
        static void reset();
        
        bool inForceVersionOkMode() const { 
            return _forceVersionOk;
        }
        
        void enterForceVersionOkMode(){ _forceVersionOk = true; }
        void leaveForceVersionOkMode(){ _forceVersionOk = false; }

    private:
        
        OID _id;
        NSVersionMap _versions;
        bool _forceVersionOk; // if this is true, then chunk version #s aren't check, and all ops are allowed

        static boost::thread_specific_ptr<ShardedConnectionInfo> _tl;
    };

    struct ShardForceVersionOkModeBlock {
        ShardForceVersionOkModeBlock(){
            info = ShardedConnectionInfo::get( false );
            if ( info )
                info->enterForceVersionOkMode();
        }
        ~ShardForceVersionOkModeBlock(){
            if ( info )
                info->leaveForceVersionOkMode();
        }

        ShardedConnectionInfo * info;
    };
    
    // -----------------
    // --- core ---
    // -----------------

    unsigned long long extractVersion( BSONElement e , string& errmsg );

    
    /**
     * @return true if we have any shard info for the ns
     */
    bool haveLocalShardingInfo( const string& ns );
    
    /**
     * @return true if the current threads shard version is ok, or not in sharded version
     */
    bool shardVersionOk( const string& ns , bool write , string& errmsg );

    /**
     * @return true if we took care of the message and nothing else should be done
     */
    bool handlePossibleShardedMessage( Message &m, DbResponse * dbresponse );

    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt );
    void aboutToDeleteForSharding( const Database* db , const DiskLoc& dl );

}
