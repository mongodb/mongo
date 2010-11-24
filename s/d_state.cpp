// @file d_state.cpp

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


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "pch.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"

#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"
#include "config.h"

using namespace std;

namespace mongo {

    // -----ShardingState START ----
    
    ShardingState::ShardingState()
        : _enabled(false) , _mutex( "ShardingState" ){
    }
    
    void ShardingState::enable( const string& server ){
        _enabled = true;
        assert( server.size() );
        if ( _configServer.size() == 0 )
            _configServer = server;
        else {
            assert( server == _configServer );
        }
    }
    
    void ShardingState::gotShardName( const string& name ){
        if ( _shardName.size() == 0 ){
            _shardName = name;
            return;
        }
        
        if ( _shardName == name )
            return;

        stringstream ss;
        ss << "gotShardName different than what i had before " 
           << " before [" << _shardName << "] " 
           << " got [" << name << "] " 
            ;
        uasserted( 13298 , ss.str() );
    }
    
    void ShardingState::gotShardHost( string host ){
        
        size_t slash = host.find( '/' );
        if ( slash != string::npos )
            host = host.substr( 0 , slash );

        if ( _shardHost.size() == 0 ){
            _shardHost = host;
            return;
        }
        
        if ( _shardHost == host )
            return;

        stringstream ss;
        ss << "gotShardHost different than what i had before " 
           << " before [" << _shardHost << "] " 
           << " got [" << host << "] " 
            ;
        uasserted( 13299 , ss.str() );
    }
    
    bool ShardingState::hasVersion( const string& ns ){
        scoped_lock lk(_mutex);

        NSVersionMap::const_iterator i = _versions.find(ns);
        return i != _versions.end();
    }
    
    bool ShardingState::hasVersion( const string& ns , ConfigVersion& version ){
        scoped_lock lk(_mutex);

        NSVersionMap::const_iterator i = _versions.find(ns);
        if ( i == _versions.end() )
            return false;
        version = i->second;
        return true;
    }
    
    const ConfigVersion ShardingState::getVersion( const string& ns ) const {
        scoped_lock lk(_mutex);

        NSVersionMap::const_iterator it = _versions.find( ns );
        if ( it != _versions.end() ) {
            return it->second;
        } else {
            return 0;
        }
    }
    
    void ShardingState::setVersion( const string& ns , const ConfigVersion& version ){
        scoped_lock lk(_mutex);

        if ( version != 0 ) {
            NSVersionMap::const_iterator it = _versions.find( ns );

            // TODO 11-18-2010 as we're bringing chunk boundary information to mongod, it may happen that
            // we're setting a version for the ns that the shard knows about already (e.g because it set
            // it itself in a chunk migration)
            // eventually, the only cases to issue a setVersion would be 
            // 1) First chunk of a collection, for version 1|0
            // 2) Drop of a collection, for version 0|0
            // 3) Load of the shard's chunk state, in a primary-secondary failover
            assert( it == _versions.end() || version >= it->second );
        }

        _versions[ns] = version;
    }

    void ShardingState::appendInfo( BSONObjBuilder& b ){
        b.appendBool( "enabled" , _enabled );
        if ( ! _enabled )
            return;

        b.append( "configServer" , _configServer );
        b.append( "shardName" , _shardName );
        b.append( "shardHost" , _shardHost );

        {
            BSONObjBuilder bb( b.subobjStart( "versions" ) );
            
            scoped_lock lk(_mutex);

            for ( NSVersionMap::iterator i=_versions.begin(); i!=_versions.end(); ++i ){
                bb.appendTimestamp( i->first , i->second );
            }
            bb.done();
        }

    }

    bool ShardingState::needShardChunkManager( const string& ns ) const {
        if ( ! _enabled )
            return false;
        
        if ( ! ShardedConnectionInfo::get( false ) )
            return false;

        return true;
    }

    ShardChunkManagerPtr ShardingState::getShardChunkManager( const string& ns ){
        ConfigVersion version;
        { 
            // check cache
            scoped_lock lk( _mutex );

            NSVersionMap::const_iterator it = _versions.find( ns );
            if ( it == _versions.end() ) {
                return ShardChunkManagerPtr();
            }

            version = it->second;

            // TODO SERVER-1849 pending drop work
            // the manager should use the cached version only if the versions match exactly
            ShardChunkManagerPtr p = _chunks[ns];
            if ( p && p->getVersion() >= version ){
                // our cached version is good, so just return
                return p;                
            }
        }

        // load the chunk information for this shard from the config database
        // a reminder: ShardChunkManager may throw on construction
        const string c = (_configServer == _shardHost) ? "" /* local */ : _configServer;
        ShardChunkManagerPtr p( new ShardChunkManager( c , ns , _shardName ) );

        // TODO SERVER-1849 verify that the manager's version is exactly the one requested
        // If not, do update _chunks, but fail the request.
        { 
            scoped_lock lk( _mutex );
            _chunks[ns] = p;
        }

        return p;
    }

    ShardingState shardingState;

    // -----ShardingState END ----
    
    // -----ShardedConnectionInfo START ----

    boost::thread_specific_ptr<ShardedConnectionInfo> ShardedConnectionInfo::_tl;

    ShardedConnectionInfo::ShardedConnectionInfo(){
        _forceVersionOk = false;
        _id.clear();
    }
    
    ShardedConnectionInfo* ShardedConnectionInfo::get( bool create ){
        ShardedConnectionInfo* info = _tl.get();
        if ( ! info && create ){
            log(1) << "entering shard mode for connection" << endl;
            info = new ShardedConnectionInfo();
            _tl.reset( info );
        }
        return info;
    }

    void ShardedConnectionInfo::reset(){
        _tl.reset();
    }

    const ConfigVersion ShardedConnectionInfo::getVersion( const string& ns ) const {
        NSVersionMap::const_iterator it = _versions.find( ns );
        if ( it != _versions.end() ) {
            return it->second;
        } else {
            return 0;
        }
    }
    
    void ShardedConnectionInfo::setVersion( const string& ns , const ConfigVersion& version ){
        _versions[ns] = version;
    }

    void ShardedConnectionInfo::setID( const OID& id ){
        _id = id;
    }

    // -----ShardedConnectionInfo END ----

    unsigned long long extractVersion( BSONElement e , string& errmsg ){
        if ( e.eoo() ){
            errmsg = "no version";
            return 0;
        }
        
        if ( e.isNumber() )
            return (unsigned long long)e.number();
        
        if ( e.type() == Date || e.type() == Timestamp )
            return e._numberLong();

        
        errmsg = "version is not a numeric type";
        return 0;
    }

    class MongodShardCommand : public Command {
    public:
        MongodShardCommand( const char * n ) : Command( n ){
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return true;
        }
    };
    
    
    bool haveLocalShardingInfo( const string& ns ){
        if ( ! shardingState.enabled() )
            return false;
        
        if ( ! shardingState.hasVersion( ns ) )
            return false;

        return ShardedConnectionInfo::get(false) > 0;
    }

    class UnsetShardingCommand : public MongodShardCommand {
    public:
        UnsetShardingCommand() : MongodShardCommand("unsetSharding"){}

        virtual void help( stringstream& help ) const {
            help << " example: { unsetSharding : 1 } ";
        }
        
        virtual LockType locktype() const { return NONE; } 
 
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            ShardedConnectionInfo::reset();
            return true;
        } 
    
    } unsetShardingCommand;

    
    class SetShardVersion : public MongodShardCommand {
    public:
        SetShardVersion() : MongodShardCommand("setShardVersion"){}

        virtual void help( stringstream& help ) const {
            help << " example: { setShardVersion : 'alleyinsider.foo' , version : 1 , configdb : '' } ";
        }
        
        virtual LockType locktype() const { return WRITE; } // TODO: figure out how to make this not need to lock
 
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){

            lastError.disableForCommand();
            ShardedConnectionInfo* info = ShardedConnectionInfo::get( true );

            bool authoritative = cmdObj.getBoolField( "authoritative" );

            string configdb = cmdObj["configdb"].valuestrsafe();
            { // configdb checking
                if ( configdb.size() == 0 ){
                    errmsg = "no configdb";
                    return false;
                }
                
                if ( shardingState.enabled() ){
                    if ( configdb != shardingState.getConfigServer() ){
                        errmsg = "specified a different configdb!";
                        return false;
                    }
                }
                else {
                    if ( ! authoritative ){
                        result.appendBool( "need_authoritative" , true );
                        errmsg = "first setShardVersion";
                        return false;
                    }
                    shardingState.enable( configdb );
                    configServer.init( configdb );
                }
            }

            if ( cmdObj["shard"].type() == String ){
                shardingState.gotShardName( cmdObj["shard"].String() );
                shardingState.gotShardHost( cmdObj["shardHost"].String() );
            }

            { // setting up ids
                if ( cmdObj["serverID"].type() != jstOID ){
                    // TODO: fix this
                    //errmsg = "need serverID to be an OID";
                    //return 0;
                }
                else {
                    OID clientId = cmdObj["serverID"].__oid();
                    if ( ! info->hasID() ){
                        info->setID( clientId );
                    }
                    else if ( clientId != info->getID() ){
                        errmsg = "server id has changed!";
                        return 0;
                    }
                }
            }

            unsigned long long version = extractVersion( cmdObj["version"] , errmsg );

            if ( errmsg.size() ){
                return false;
            }
            
            string ns = cmdObj["setShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need to speciy fully namespace";
                return false;
            }
            
            const ConfigVersion oldVersion = info->getVersion(ns);
            const ConfigVersion globalVersion = shardingState.getVersion(ns);
            
            if ( oldVersion > 0 && globalVersion == 0 ){
                // this had been reset
                info->setVersion( ns , 0 );
            }

            if ( version == 0 && globalVersion == 0 ){
                // this connection is cleaning itself
                info->setVersion( ns , 0 );
                return true;
            }

            if ( version == 0 && globalVersion > 0 ){
                if ( ! authoritative ){
                    result.appendBool( "need_authoritative" , true );
                    result.appendTimestamp( "globalVersion" , globalVersion );
                    result.appendTimestamp( "oldVersion" , oldVersion );
                    errmsg = "dropping needs to be authoritative";
                    return false;
                }
                log() << "wiping data for: " << ns << endl;
                result.appendTimestamp( "beforeDrop" , globalVersion );
                // only setting global version on purpose
                // need clients to re-find meta-data
                shardingState.setVersion( ns , 0 );
                info->setVersion( ns , 0 );
                return true;
            }

            if ( version < oldVersion ){
                errmsg = "you already have a newer version";
                result.appendTimestamp( "oldVersion" , oldVersion );
                result.appendTimestamp( "newVersion" , version );
                result.appendTimestamp( "globalVersion" , globalVersion );
                return false;
            }
            
            if ( version < globalVersion ){
                while ( shardingState.inCriticalMigrateSection() ){
                    dbtemprelease r;
                    sleepmillis(2);
                    log() << "waiting till out of critical section" << endl;
                }
                errmsg = "going to older version for global";
                result.appendTimestamp( "version" , version );
                result.appendTimestamp( "globalVersion" , globalVersion );
                return false;
            }
            
            if ( globalVersion == 0 && ! cmdObj.getBoolField( "authoritative" ) ){
                // need authoritative for first look
                result.appendBool( "need_authoritative" , true );
                result.append( "ns" , ns );
                errmsg = "first time for this ns";
                return false;
            }

            result.appendTimestamp( "oldVersion" , oldVersion );
            result.append( "ok" , 1 );

            info->setVersion( ns , version );
            shardingState.setVersion( ns , version );

            // TODO SERVER-1849 pending drop work
            // getShardChunkManager is assuming that the setVersion above were valid
            // ideally, we'd call getShardChunkManager first, verify that 'version' is sound, and then update
            // connection and global state
            {
                dbtemprelease unlock;
                shardingState.getShardChunkManager( ns );
            }

            return true;
        }
        
    } setShardVersionCmd;
    
    class GetShardVersion : public MongodShardCommand {
    public:
        GetShardVersion() : MongodShardCommand("getShardVersion"){}

        virtual void help( stringstream& help ) const {
            help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
        }
        
        virtual LockType locktype() const { return NONE; } 

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            string ns = cmdObj["getShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need to speciy fully namespace";
                return false;
            }
            
            result.append( "configServer" , shardingState.getConfigServer() );

            result.appendTimestamp( "global" , shardingState.getVersion(ns) );
            
            ShardedConnectionInfo* info = ShardedConnectionInfo::get( false );
            if ( info )
                result.appendTimestamp( "mine" , info->getVersion(ns) );
            else 
                result.appendTimestamp( "mine" , 0 );
            
            return true;
        }
        
    } getShardVersion;

    class ShardingStateCmd : public MongodShardCommand {
    public:
        ShardingStateCmd() : MongodShardCommand( "shardingState" ){}

        virtual LockType locktype() const { return WRITE; } // TODO: figure out how to make this not need to lock

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){        
            shardingState.appendInfo( result );
            return true;
        }
        
    } shardingStateCmd;

    /**
     * @ return true if not in sharded mode
                     or if version for this client is ok
     */
    bool shardVersionOk( const string& ns , bool isWriteOp , string& errmsg ){
        if ( ! shardingState.enabled() )
            return true;

        ShardedConnectionInfo* info = ShardedConnectionInfo::get( false );

        if ( ! info ){
            // this means the client has nothing sharded
            // so this allows direct connections to do whatever they want
            // which i think is the correct behavior
            return true;
        }
        
        if ( info->inForceVersionOkMode() ){
            return true;
        }

        ConfigVersion version;    
        if ( ! shardingState.hasVersion( ns , version ) ){
            return true;
        }

        ConfigVersion clientVersion = info->getVersion(ns);

        if ( version == 0 && clientVersion > 0 ){
            stringstream ss;
            ss << "collection was dropped or this shard no longer valied version: " << version << " clientVersion: " << clientVersion;
            errmsg = ss.str();
            return false;
        }
        
        if ( clientVersion >= version )
            return true;
        

        if ( clientVersion == 0 ){
            stringstream ss;
            ss << "client in sharded mode, but doesn't have version set for this collection: " << ns << " myVersion: " << version;
            errmsg = ss.str();
            return false;
        }

        if ( isWriteOp && version.majorVersion() == clientVersion.majorVersion() ){
            // this means there was just a split 
            // since on a split w/o a migrate this server is ok
            // going to accept write
            return true;
        }

        stringstream ss;
        ss << "your version is too old  ns: " + ns << " global: " << version << " client: " << clientVersion;
        errmsg = ss.str();
        return false;
    }

}
