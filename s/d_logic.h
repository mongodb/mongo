// d_logic.h
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

namespace mongo {
    
    typedef unsigned long long ConfigVersion;
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

        
        bool hasVersion( const string& ns ) const;
        bool hasVersion( const string& ns , ConfigVersion& version ) const;
        ConfigVersion& getVersion( const string& ns ); // TODO: this is dangeroues
        void setVersion( const string& ns , const ConfigVersion& version );
        
    private:
        
        bool _enabled;
        string _configServer;
        NSVersionMap _versions;
    };
    
    extern ShardingState shardingState;

    // --------------
    // --- per connection ---
    // --------------
    
    class ShardedConnectionInfo {
    public:
        ShardedConnectionInfo();
        
        const OID& getID() const { return _id; }
        bool hasID() const { return _id.isSet(); }
        void setID( const OID& id );
        
        ConfigVersion& getVersion( const string& ns ); // TODO: this is dangeroues
        void setVersion( const string& ns , const ConfigVersion& version );
        
        static ShardedConnectionInfo* get( bool create );
        
    private:
        
        OID _id;
        NSVersionMap _versions;

        static boost::thread_specific_ptr<ShardedConnectionInfo> _tl;
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
    bool shardVersionOk( const string& ns , string& errmsg );

    /**
     * @return true if we took care of the message and nothing else should be done
     */
    bool handlePossibleShardedMessage( Message &m, DbResponse &dbresponse );

    // -----------------
    // --- writeback ---
    // -----------------

    /* queue a write back on a remote server for a failed write */
    void queueWriteBack( const string& remote , const BSONObj& o );

}
