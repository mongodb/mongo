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
#include "../db/db.h"
#include "../db/replutil.h"
#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"
#include "config.h"
#include "mongo/util/concurrency/ticketholder.h"

using namespace std;

namespace mongo {

    // -----ShardingState START ----

    ShardingState::ShardingState()
        : _enabled(false) , _mutex( "ShardingState" ),
          _configServerTickets( 3 /* max number of concurrent config server refresh threads */ ) {
    }

    void ShardingState::enable( const string& server ) {
        _enabled = true;
        verify( server.size() );
        if ( _configServer.size() == 0 )
            _configServer = server;
        else {
            verify( server == _configServer );
        }
    }

    void ShardingState::gotShardName( const string& name ) {
        scoped_lock lk(_mutex);
        if ( _shardName.size() == 0 ) {
            // TODO SERVER-2299 verify the name is sound w.r.t IPs
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
        msgasserted( 13298 , ss.str() );
    }

    void ShardingState::gotShardHost( string host ) {
        scoped_lock lk(_mutex);
        size_t slash = host.find( '/' );
        if ( slash != string::npos )
            host = host.substr( 0 , slash );

        if ( _shardHost.size() == 0 ) {
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
        msgasserted( 13299 , ss.str() );
    }

    void ShardingState::resetShardingState() {
        scoped_lock lk(_mutex);
        
        _enabled = false;
        _configServer.clear();
        _shardName.clear();
        _shardHost.clear();
        _chunks.clear();
    }

    // TODO we shouldn't need three ways for checking the version. Fix this.
    bool ShardingState::hasVersion( const string& ns ) {
        scoped_lock lk(_mutex);

        ChunkManagersMap::const_iterator it = _chunks.find(ns);
        return it != _chunks.end();
    }

    bool ShardingState::hasVersion( const string& ns , ConfigVersion& version ) {
        scoped_lock lk(_mutex);

        ChunkManagersMap::const_iterator it = _chunks.find(ns);
        if ( it == _chunks.end() )
            return false;

        ShardChunkManagerPtr p = it->second;
        version = p->getVersion();
        return true;
    }

    const ConfigVersion ShardingState::getVersion( const string& ns ) const {
        scoped_lock lk(_mutex);

        ChunkManagersMap::const_iterator it = _chunks.find( ns );
        if ( it != _chunks.end() ) {
            ShardChunkManagerPtr p = it->second;
            return p->getVersion();
        }
        else {
            return 0;
        }
    }

    void ShardingState::donateChunk( const string& ns , const BSONObj& min , const BSONObj& max , ShardChunkVersion version ) {
        scoped_lock lk( _mutex );

        ChunkManagersMap::const_iterator it = _chunks.find( ns );
        verify( it != _chunks.end() ) ;
        ShardChunkManagerPtr p = it->second;

        // empty shards should have version 0
        version = ( p->getNumChunks() > 1 ) ? version : ShardChunkVersion( 0 , 0 );

        ShardChunkManagerPtr cloned( p->cloneMinus( min , max , version ) );
        _chunks[ns] = cloned;
    }

    void ShardingState::undoDonateChunk( const string& ns , const BSONObj& min , const BSONObj& max , ShardChunkVersion version ) {
        scoped_lock lk( _mutex );

        ChunkManagersMap::const_iterator it = _chunks.find( ns );
        verify( it != _chunks.end() ) ;
        ShardChunkManagerPtr p( it->second->clonePlus( min , max , version ) );
        _chunks[ns] = p;
    }

    void ShardingState::splitChunk( const string& ns , const BSONObj& min , const BSONObj& max , const vector<BSONObj>& splitKeys ,
                                    ShardChunkVersion version ) {
        scoped_lock lk( _mutex );

        ChunkManagersMap::const_iterator it = _chunks.find( ns );
        verify( it != _chunks.end() ) ;
        ShardChunkManagerPtr p( it->second->cloneSplit( min , max , splitKeys , version ) );
        _chunks[ns] = p;
    }

    void ShardingState::resetVersion( const string& ns ) {
        scoped_lock lk( _mutex );

        _chunks.erase( ns );
    }

    bool ShardingState::trySetVersion( const string& ns , ConfigVersion& version /* IN-OUT */ ) {

        // Currently this function is called after a getVersion(), which is the first "check", and the assumption here
        // is that we don't do anything nearly as long as a remote query in a thread between then and now.
        // Otherwise it may be worth adding an additional check without the _configServerMutex below, since then it
        // would be likely that the version may have changed in the meantime without waiting for or fetching config results.

        // TODO:  Mutex-per-namespace?
        
        LOG( 2 ) << "trying to set shard version of " << version.toString() << " for '" << ns << "'" << endl;
        
        _configServerTickets.waitForTicket();
        TicketHolderReleaser needTicketFrom( &_configServerTickets );

        // fast path - double-check if requested version is at the same version as this chunk manager before verifying
        // against config server
        //
        // This path will short-circuit the version set if another thread already managed to update the version in the
        // meantime.  First check is from getVersion().
        //
        // cases:
        //   + this shard updated the version for a migrate's commit (FROM side)
        //     a client reloaded chunk state from config and picked the newest version
        //   + two clients reloaded
        //     one triggered the 'slow path' (below)
        //     when the second's request gets here, the version is already current
        ConfigVersion storedVersion;
        {
            scoped_lock lk( _mutex );
            ChunkManagersMap::const_iterator it = _chunks.find( ns );
            if ( it != _chunks.end() && ( storedVersion = it->second->getVersion() ) == version )
                return true;
        }
        
        LOG( 2 ) << "verifying cached version " << storedVersion.toString() << " and new version " << version.toString() << " for '" << ns << "'" << endl;

        // slow path - requested version is different than the current chunk manager's, if one exists, so must check for
        // newest version in the config server
        //
        // cases:
        //   + a chunk moved TO here
        //     (we don't bump up the version on the TO side but the commit to config does use higher version)
        //     a client reloads from config an issued the request
        //   + there was a take over from a secondary
        //     the secondary had no state (managers) at all, so every client request will fall here
        //   + a stale client request a version that's not current anymore

        // Can't lock default mutex while creating ShardChunkManager, b/c may have to create a new connection to myself
        const string c = (_configServer == _shardHost) ? "" /* local */ : _configServer;
        ShardChunkManagerPtr p( new ShardChunkManager( c , ns , _shardName ) );

        {
            scoped_lock lk( _mutex );

            // since we loaded the chunk manager unlocked, other thread may have done the same
            // make sure we keep the freshest config info only
            ChunkManagersMap::const_iterator it = _chunks.find( ns );
            if ( it == _chunks.end() || p->getVersion() >= it->second->getVersion() ) {
                _chunks[ns] = p;
            }

            ShardChunkVersion oldVersion = version;
            version = p->getVersion();
            return oldVersion == version;
        }
    }

    void ShardingState::appendInfo( BSONObjBuilder& b ) {
        b.appendBool( "enabled" , _enabled );
        if ( ! _enabled )
            return;

        b.append( "configServer" , _configServer );
        b.append( "shardName" , _shardName );
        b.append( "shardHost" , _shardHost );

        {
            BSONObjBuilder bb( b.subobjStart( "versions" ) );

            scoped_lock lk(_mutex);

            for ( ChunkManagersMap::iterator it = _chunks.begin(); it != _chunks.end(); ++it ) {
                ShardChunkManagerPtr p = it->second;
                bb.appendTimestamp( it->first , p->getVersion() );
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

    ShardChunkManagerPtr ShardingState::getShardChunkManager( const string& ns ) {
        scoped_lock lk( _mutex );

        ChunkManagersMap::const_iterator it = _chunks.find( ns );
        if ( it == _chunks.end() ) {
            return ShardChunkManagerPtr();
        }
        else {
            return it->second;
        }
    }

    ShardingState shardingState;

    // -----ShardingState END ----

    // -----ShardedConnectionInfo START ----

    boost::thread_specific_ptr<ShardedConnectionInfo> ShardedConnectionInfo::_tl;

    ShardedConnectionInfo::ShardedConnectionInfo() {
        _forceVersionOk = false;
        _id.clear();
    }

    ShardedConnectionInfo* ShardedConnectionInfo::get( bool create ) {
        ShardedConnectionInfo* info = _tl.get();
        if ( ! info && create ) {
            LOG(1) << "entering shard mode for connection" << endl;
            info = new ShardedConnectionInfo();
            _tl.reset( info );
        }
        return info;
    }

    void ShardedConnectionInfo::reset() {
        _tl.reset();
    }

    const ConfigVersion ShardedConnectionInfo::getVersion( const string& ns ) const {
        NSVersionMap::const_iterator it = _versions.find( ns );
        if ( it != _versions.end() ) {
            return it->second;
        }
        else {
            return 0;
        }
    }

    void ShardedConnectionInfo::setVersion( const string& ns , const ConfigVersion& version ) {
        _versions[ns] = version;
    }

    void ShardedConnectionInfo::addHook() {
        static bool done = false;
        if (!done) {
            LOG(1) << "adding sharding hook" << endl;
            pool.addHook(new ShardingConnectionHook(false));
            shardConnectionPool.addHook(new ShardingConnectionHook(true));
            done = true;
        }
    }

    void ShardedConnectionInfo::setID( const OID& id ) {
        _id = id;
    }

    // -----ShardedConnectionInfo END ----

    unsigned long long extractVersion( BSONElement e , string& errmsg ) {
        if ( e.eoo() ) {
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
        MongodShardCommand( const char * n ) : Command( n ) {
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool adminOnly() const {
            return true;
        }
    };


    bool haveLocalShardingInfo( const string& ns ) {
        if ( ! shardingState.enabled() )
            return false;

        if ( ! shardingState.hasVersion( ns ) )
            return false;

        return ShardedConnectionInfo::get(false) > 0;
    }

    class UnsetShardingCommand : public MongodShardCommand {
    public:
        UnsetShardingCommand() : MongodShardCommand("unsetSharding") {}

        virtual void help( stringstream& help ) const {
            help << " example: { unsetSharding : 1 } ";
        }

        virtual LockType locktype() const { return NONE; }

        virtual bool slaveOk() const { return true; }

        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            ShardedConnectionInfo::reset();
            return true;
        }

    } unsetShardingCommand;

    class SetShardVersion : public MongodShardCommand {
    public:
        SetShardVersion() : MongodShardCommand("setShardVersion") {}

        virtual void help( stringstream& help ) const {
            help << " example: { setShardVersion : 'alleyinsider.foo' , version : 1 , configdb : '' } ";
        }

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        
        bool checkConfigOrInit( const string& configdb , bool authoritative , string& errmsg , BSONObjBuilder& result , bool locked=false ) const {
            if ( configdb.size() == 0 ) {
                errmsg = "no configdb";
                return false;
            }
            
            if ( shardingState.enabled() ) {
                if ( configdb == shardingState.getConfigServer() ) 
                    return true;
                
                result.append( "configdb" , BSON( "stored" << shardingState.getConfigServer() << 
                                                  "given" << configdb ) );
                errmsg = "specified a different configdb!";
                return false;
            }
            
            if ( ! authoritative ) {
                result.appendBool( "need_authoritative" , true );
                errmsg = "first setShardVersion";
                return false;
            }
            
            if ( locked ) {
                ShardedConnectionInfo::addHook();
                shardingState.enable( configdb );
                configServer.init( configdb );
                return true;
            }

            Lock::GlobalWrite lk;
            return checkConfigOrInit( configdb , authoritative , errmsg , result , true );
        }
        
        bool checkMongosID( ShardedConnectionInfo* info, const BSONElement& id, string& errmsg ) {
            if ( id.type() != jstOID ) {
                if ( ! info->hasID() ) {
                    warning() << "bad serverID set in setShardVersion and none in info: " << id << endl;
                }
                // TODO: fix this
                //errmsg = "need serverID to be an OID";
                //return 0;
                return true;
            }
            
            OID clientId = id.__oid();
            if ( ! info->hasID() ) {
                info->setID( clientId );
                return true;
            }
            
            if ( clientId != info->getID() ) {
                errmsg = "server id has changed!";
                return false;
            }

            return true;
        }

        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

            // Steps
            // 1. check basic config
            // 2. extract params from command
            // 3. fast check
            // 4. slow check (LOCKS)
            
            // step 1

            lastError.disableForCommand();
            ShardedConnectionInfo* info = ShardedConnectionInfo::get( true );

            // make sure we have the mongos id for writebacks
            if ( ! checkMongosID( info , cmdObj["serverID"] , errmsg ) ) 
                return false;

            bool authoritative = cmdObj.getBoolField( "authoritative" );
            
            // check config server is ok or enable sharding
            if ( ! checkConfigOrInit( cmdObj["configdb"].valuestrsafe() , authoritative , errmsg , result ) )
                return false;

            // check shard name/hosts are correct
            if ( cmdObj["shard"].type() == String ) {
                shardingState.gotShardName( cmdObj["shard"].String() );
                shardingState.gotShardHost( cmdObj["shardHost"].String() );
            }
            

            // Handle initial shard connection
            if( cmdObj["version"].eoo() && cmdObj["init"].trueValue() ){
                result.append( "initialized", true );
                return true;
            }

            // we can run on a slave up to here
            if ( ! isMaster( "admin" ) ) {
                result.append( "errmsg" , "not master" );
                result.append( "note" , "from post init in setShardVersion" );
                return false;
            }

            // step 2
            
            string ns = cmdObj["setShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ) {
                errmsg = "need to specify namespace";
                return false;
            }

            const ConfigVersion version = extractVersion( cmdObj["version"] , errmsg );
            if ( errmsg.size() )
                return false;
            
            // step 3

            const ConfigVersion oldVersion = info->getVersion(ns);
            const ConfigVersion globalVersion = shardingState.getVersion(ns);

            result.appendTimestamp( "oldVersion" , oldVersion );
            
            if ( globalVersion > 0 && version > 0 ) {
                // this means there is no reset going on an either side
                // so its safe to make some assumptions

                if ( version == globalVersion ) {
                    // mongos and mongod agree!
                    if ( oldVersion != version ) {
                        if ( oldVersion < globalVersion ) {
                            info->setVersion( ns , version );
                        }
                        else if ( authoritative ) {
                            // this means there was a drop and our version is reset
                            info->setVersion( ns , version );
                        }
                        else {
                            result.append( "ns" , ns );
                            result.appendBool( "need_authoritative" , true );
                            errmsg = "verifying drop on '" + ns + "'";
                            return false;
                        }
                    }
                    return true;
                }
                
            }

            // step 4
            
            // this is because of a weird segfault I saw and I can't see why this should ever be set
            massert( 13647 , str::stream() << "context should be empty here, is: " << cc().getContext()->ns() , cc().getContext() == 0 ); 
        
            Lock::GlobalWrite setShardVersionLock; // TODO: can we get rid of this??
            
            if ( oldVersion > 0 && globalVersion == 0 ) {
                // this had been reset
                info->setVersion( ns , 0 );
            }

            if ( version == 0 && globalVersion == 0 ) {
                // this connection is cleaning itself
                info->setVersion( ns , 0 );
                return true;
            }

            if ( version == 0 && globalVersion > 0 ) {
                if ( ! authoritative ) {
                    result.appendBool( "need_authoritative" , true );
                    result.append( "ns" , ns );
                    result.appendTimestamp( "globalVersion" , globalVersion );
                    errmsg = "dropping needs to be authoritative";
                    return false;
                }
                log() << "wiping data for: " << ns << endl;
                result.appendTimestamp( "beforeDrop" , globalVersion );
                // only setting global version on purpose
                // need clients to re-find meta-data
                shardingState.resetVersion( ns );
                info->setVersion( ns , 0 );
                return true;
            }

            if ( version < oldVersion ) {
                errmsg = "this connection already had a newer version of collection '" + ns + "'";
                result.append( "ns" , ns );
                result.appendTimestamp( "newVersion" , version );
                result.appendTimestamp( "globalVersion" , globalVersion );
                return false;
            }

            if ( version < globalVersion ) {
                while ( shardingState.inCriticalMigrateSection() ) {
                    dbtemprelease r;
                    sleepmillis(2);
                    OCCASIONALLY log() << "waiting till out of critical section" << endl;
                }
                errmsg = "shard global version for collection is higher than trying to set to '" + ns + "'";
                result.append( "ns" , ns );
                result.appendTimestamp( "version" , version );
                result.appendTimestamp( "globalVersion" , globalVersion );
                result.appendBool( "reloadConfig" , true );
                return false;
            }

            if ( globalVersion == 0 && ! authoritative ) {
                // Needed b/c when the last chunk is moved off a shard, the version gets reset to zero, which
                // should require a reload.
                // TODO: Maybe a more elegant way of doing this
                while ( shardingState.inCriticalMigrateSection() ) {
                    dbtemprelease r;
                    sleepmillis(2);
                    OCCASIONALLY log() << "waiting till out of critical section for version reset" << endl;
                }

                // need authoritative for first look
                result.append( "ns" , ns );
                result.appendBool( "need_authoritative" , true );
                errmsg = "first time for collection '" + ns + "'";
                return false;
            }

            Timer relockTime;
            {
                dbtemprelease unlock;

                ShardChunkVersion currVersion = version;
                if ( ! shardingState.trySetVersion( ns , currVersion ) ) {
                    errmsg = str::stream() << "client version differs from config's for collection '" << ns << "'";
                    result.append( "ns" , ns );
                    result.appendTimestamp( "version" , version );
                    result.appendTimestamp( "globalVersion" , currVersion );
                    return false;
                }
            }
            if ( relockTime.millis() >= ( cmdLine.slowMS - 10 ) ) {
                log() << "setShardVersion - relocking slow: " << relockTime.millis() << endl;
            }
            
            info->setVersion( ns , version );
            return true;
        }

    } setShardVersionCmd;

    class GetShardVersion : public MongodShardCommand {
    public:
        GetShardVersion() : MongodShardCommand("getShardVersion") {}

        virtual void help( stringstream& help ) const {
            help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
        }

        virtual LockType locktype() const { return NONE; }

        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string ns = cmdObj["getShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ) {
                errmsg = "need to specify full namespace";
                return false;
            }

            result.append( "configServer" , shardingState.getConfigServer() );

            result.appendTimestamp( "global" , shardingState.getVersion(ns) );

            ShardedConnectionInfo* info = ShardedConnectionInfo::get( false );
            result.appendBool( "inShardedMode" , info != 0 );
            if ( info )
                result.appendTimestamp( "mine" , info->getVersion(ns) );
            else
                result.appendTimestamp( "mine" , 0 );

            return true;
        }

    } getShardVersion;

    class ShardingStateCmd : public MongodShardCommand {
    public:
        ShardingStateCmd() : MongodShardCommand( "shardingState" ) {}

        virtual LockType locktype() const { return WRITE; } // TODO: figure out how to make this not need to lock

        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            shardingState.appendInfo( result );
            return true;
        }

    } shardingStateCmd;

    /**
     * @ return true if not in sharded mode
                     or if version for this client is ok
     */
    bool shardVersionOk( const string& ns , string& errmsg, ConfigVersion& received, ConfigVersion& wanted ) {
        if ( ! shardingState.enabled() )
            return true;

        if ( ! isMasterNs( ns.c_str() ) )  {
            // right now connections to secondaries aren't versioned at all
            return true;
        }

        ShardedConnectionInfo* info = ShardedConnectionInfo::get( false );

        if ( ! info ) {
            // this means the client has nothing sharded
            // so this allows direct connections to do whatever they want
            // which i think is the correct behavior
            return true;
        }

        if ( info->inForceVersionOkMode() ) {
            return true;
        }

        // TODO
        //   all collections at some point, be sharded or not, will have a version (and a ShardChunkManager)
        //   for now, we remove the sharding state of dropped collection
        //   so delayed request may come in. This has to be fixed.
        ConfigVersion clientVersion = info->getVersion(ns);
        ConfigVersion version;
        if ( ! shardingState.hasVersion( ns , version ) && clientVersion == 0 ) {
            return true;
        }

        // The versions we're going to compare, saved for future use
        received = clientVersion;
        wanted = version;

        if ( version == 0 && clientVersion > 0 ) {
            stringstream ss;
            ss << "collection was dropped or this shard no longer valid version";
            errmsg = ss.str();
            return false;
        }

        if ( clientVersion >= version )
            return true;


        if ( clientVersion == 0 ) {
            stringstream ss;
            ss << "client in sharded mode, but doesn't have version set for this collection";
            errmsg = ss.str();
            return false;
        }

        if ( version.majorVersion() == clientVersion.majorVersion() ) {
            // this means there was just a split
            // since on a split w/o a migrate this server is ok
            // going to accept 
            return true;
        }

        stringstream ss;
        ss << "your version is too old";
        errmsg = ss.str();
        return false;
    }

    void ShardingConnectionHook::onHandedOut( DBClientBase * conn ) {
        // no-op for mongod
    }
}
