// d_state.cpp

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
    
    void ShardingState::gotShardHost( const string& host ){
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
    
    ConfigVersion& ShardingState::getVersion( const string& ns ){
        scoped_lock lk(_mutex);
        return _versions[ns];
    }
    
    void ShardingState::setVersion( const string& ns , const ConfigVersion& version ){
        scoped_lock lk(_mutex);
        ConfigVersion& me = _versions[ns];
        assert( version == 0 || version > me );
        me = version;
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
                bb.appendTimestamp( i->first.c_str() , i->second );
            }
            bb.done();
        }

    }

    ChunkMatcherPtr ShardingState::getChunkMatcher( const string& ns ){
        if ( ! _enabled )
            return ChunkMatcherPtr();
        
        if ( ! ShardedConnectionInfo::get( false ) )
            return ChunkMatcherPtr();

        ConfigVersion version;
        {
            scoped_lock lk( _mutex );
            version = _versions[ns];
            
            if ( ! version )
                return ChunkMatcherPtr();
            
            ChunkMatcherPtr p = _chunks[ns];
            if ( p && p->_version >= version )
                return p;                
        }

        BSONObj q;
        {
            BSONObjBuilder b;
            b.append( "ns" , ns.c_str() );
            b.append( "shard" , BSON( "$in" << BSON_ARRAY( _shardHost << _shardName ) ) );
            q = b.obj();
        }

        auto_ptr<ScopedDbConnection> scoped;
        auto_ptr<DBDirectClient> direct;
        
        DBClientBase * conn;

        if ( _configServer == _shardHost ){
            direct.reset( new DBDirectClient() );
            conn = direct.get();
        }
        else {
            scoped.reset( new ScopedDbConnection( _configServer ) );
            conn = scoped->get();
        }

        auto_ptr<DBClientCursor> cursor = conn->query( "config.chunks" , Query(q).sort( "min" ) );
        assert( cursor.get() );
        if ( ! cursor->more() ){
            if ( scoped.get() )
                scoped->done();
            return ChunkMatcherPtr();
        }
        
        ChunkMatcherPtr p( new ChunkMatcher( version ) );
        
        BSONObj min,max;
        while ( cursor->more() ){
            BSONObj d = cursor->next();
            
            if ( min.isEmpty() ){
                min = d["min"].Obj().getOwned();
                max = d["max"].Obj().getOwned();
                continue;
            }

            if ( max == d["min"].Obj() ){
                max = d["max"].Obj().getOwned();
                continue;
            }

            p->gotRange( min.getOwned() , max.getOwned() );
            min = d["min"].Obj().getOwned();
            max = d["max"].Obj().getOwned();
        }
        assert( ! min.isEmpty() );
        p->gotRange( min.getOwned() , max.getOwned() );
        
        if ( scoped.get() )
            scoped->done();

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
        _forceMode = false;
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

    ConfigVersion& ShardedConnectionInfo::getVersion( const string& ns ){
        return _versions[ns];
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

            // Debugging code for SERVER-1633. Commands have already a coarser timer for
            // normal operation.
            Timer timer;
            vector<int> laps;

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

            // SERVER-1633
            laps.push_back( timer.millis() );
            
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

            // SERVER-1633
            laps.push_back( timer.millis() );
            
            unsigned long long version = extractVersion( cmdObj["version"] , errmsg );

            if ( errmsg.size() ){
                return false;
            }
            
            string ns = cmdObj["setShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need to speciy fully namespace";
                return false;
            }
            
            ConfigVersion& oldVersion = info->getVersion(ns);
            ConfigVersion& globalVersion = shardingState.getVersion(ns);
            
            if ( oldVersion > 0 && globalVersion == 0 ){
                // this had been reset
                oldVersion = 0;
            }

            if ( version == 0 && globalVersion == 0 ){
                // this connection is cleaning itself
                oldVersion = 0;
                return 1;
            }

            // SERVER-1633
            laps.push_back( timer.millis() );

            if ( version == 0 && globalVersion > 0 ){
                if ( ! authoritative ){
                    result.appendBool( "need_authoritative" , true );
                    result.appendTimestamp( "globalVersion" , globalVersion );
                    result.appendTimestamp( "oldVersion" , oldVersion );
                    errmsg = "dropping needs to be authoritative";
                    return 0;
                }
                log() << "wiping data for: " << ns << endl;
                result.appendTimestamp( "beforeDrop" , globalVersion );
                // only setting global version on purpose
                // need clients to re-find meta-data
                globalVersion = 0;
                oldVersion = 0;
                return 1;
            }

            if ( version < oldVersion ){
                errmsg = "you already have a newer version";
                result.appendTimestamp( "oldVersion" , oldVersion );
                result.appendTimestamp( "newVersion" , version );
                result.appendTimestamp( "globalVersion" , globalVersion );
                return false;
            }
            
            // SERVER-1633
            laps.push_back( timer.millis() );

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

            // SERVER-1633
            laps.push_back( timer.millis() );

            {
                dbtemprelease unlock;
                shardingState.getChunkMatcher( ns );
            }

            result.appendTimestamp( "oldVersion" , oldVersion );
            oldVersion = version;
            globalVersion = version;

            // SERVER-1633
            ostringstream lapString;
            lapString << name /* command name */ << " partials: " ;
            for (size_t i = 1; i<laps.size(); ++i){ 
                lapString << (laps[i] - laps[i-1]) / 1000 << " ";
            }
            lapString << endl;
            logIfSlow( timer, lapString.str() );

            result.append( "ok" , 1 );
            return 1;
        }
        
    } setShardVersion;
    
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
        
        if ( info->inForceMode() ){
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

    // --- ChunkMatcher ---

    ChunkMatcher::ChunkMatcher( ConfigVersion version )
        : _version( version ){

    }

    void ChunkMatcher::gotRange( const BSONObj& min , const BSONObj& max ){
        if (_key.isEmpty()){
            BSONObjBuilder b;

            BSONForEach(e, min) {
                b.append(e.fieldName(), 1);
            }

            _key = b.obj();
        }

        //TODO debug mode only?
        assert(min.nFields() == _key.nFields());
        assert(max.nFields() == _key.nFields());

        _map[min] = make_pair(min,max);
    }

    bool ChunkMatcher::belongsToMe( const BSONObj& key , const DiskLoc& loc ) const {
        if ( _map.size() == 0 )
            return false;
        
        BSONObj x = loc.obj().extractFields(_key);
        
        MyMap::const_iterator a = _map.upper_bound( x );
        if ( a != _map.begin() )
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
    
}
