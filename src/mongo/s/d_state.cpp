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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "mongo/pch.h"

#include <map>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/db.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/client/connpool.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/config.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/metadata_loader.h"
#include "mongo/s/shard.h"
#include "mongo/util/queue.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"

using namespace std;

namespace mongo {

    // -----ShardingState START ----

    ShardingState::ShardingState()
        : _enabled(false) , _mutex( "ShardingState" ),
          _configServerTickets( 3 /* max number of concurrent config server refresh threads */ ) {
    }

    void ShardingState::enable( const string& server ) {
        scoped_lock lk(_mutex);

        _enabled = true;
        verify( server.size() );
        if ( _configServer.size() == 0 )
            _configServer = server;
        else {
            verify( server == _configServer );
        }
    }

    void ShardingState::initialize(const string& server) {
        ShardedConnectionInfo::addHook();
        shardingState.enable(server);
        configServer.init(server);
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
        _collMetadata.clear();
    }

    // TODO we shouldn't need three ways for checking the version. Fix this.
    bool ShardingState::hasVersion( const string& ns ) {
        scoped_lock lk(_mutex);

        CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
        return it != _collMetadata.end();
    }

    bool ShardingState::hasVersion( const string& ns , ChunkVersion& version ) {
        scoped_lock lk(_mutex);

        CollectionMetadataMap::const_iterator it = _collMetadata.find(ns);
        if ( it == _collMetadata.end() )
            return false;

        CollectionMetadataPtr p = it->second;
        version = p->getShardVersion();
        return true;
    }

    const ChunkVersion ShardingState::getVersion( const string& ns ) const {
        scoped_lock lk(_mutex);

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it != _collMetadata.end() ) {
            CollectionMetadataPtr p = it->second;
            return p->getShardVersion();
        }
        else {
            return ChunkVersion( 0, OID() );
        }
    }

    void ShardingState::donateChunk( const string& ns , const BSONObj& min , const BSONObj& max , ChunkVersion version ) {
        scoped_lock lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() ) ;
        CollectionMetadataPtr p = it->second;

        // empty shards should have version 0
        version =
                ( p->getNumChunks() > 1 ) ?
                        version : ChunkVersion( 0, 0, p->getCollVersion().epoch() );

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );
        string errMsg;

        CollectionMetadataPtr cloned( p->cloneMigrate( chunk, version, &errMsg ) );
        // uassert to match old behavior, TODO: report errors w/o throwing
        uassert( 16855, errMsg, NULL != cloned.get() );

        // TODO: a bit dangerous to have two different zero-version states - no-metadata and
        // no-version
        _collMetadata[ns] = cloned;
    }

    void ShardingState::undoDonateChunk( const string& ns, CollectionMetadataPtr prevMetadata ) {
        scoped_lock lk( _mutex );
        log() << "ShardingState::undoDonateChunk acquired _mutex" << endl;

        CollectionMetadataMap::iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() );
        it->second = prevMetadata;
    }

    bool ShardingState::notePending( const string& ns,
                                     const BSONObj& min,
                                     const BSONObj& max,
                                     const OID& epoch,
                                     string* errMsg ) {
        scoped_lock lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it == _collMetadata.end() ) {

            *errMsg = str::stream() << "could not note chunk " << "[" << min << "," << max << ")"
                                    << " as pending because the local metadata for " << ns
                                    << " has changed";

            return false;
        }

        CollectionMetadataPtr metadata = it->second;

        // This can currently happen because drops aren't synchronized with in-migrations
        // The idea for checking this here is that in the future we shouldn't have this problem
        if ( metadata->getCollVersion().epoch() != epoch ) {

            *errMsg = str::stream() << "could not note chunk " << "[" << min << "," << max << ")"
                                    << " as pending because the epoch for " << ns
                                    << " has changed from "
                                    << epoch << " to " << metadata->getCollVersion().epoch();

            return false;
        }

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );

        CollectionMetadataPtr cloned( metadata->clonePlusPending( chunk, errMsg ) );
        if ( !cloned ) return false;

        _collMetadata[ns] = cloned;
        return true;
    }

    bool ShardingState::forgetPending( const string& ns,
                                       const BSONObj& min,
                                       const BSONObj& max,
                                       const OID& epoch,
                                       string* errMsg ) {
        scoped_lock lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it == _collMetadata.end() ) {

            *errMsg = str::stream() << "no need to forget pending chunk "
                                    << "[" << min << "," << max << ")"
                                    << " because the local metadata for " << ns << " has changed";

            return false;
        }

        CollectionMetadataPtr metadata = it->second;

        // This can currently happen because drops aren't synchronized with in-migrations
        // The idea for checking this here is that in the future we shouldn't have this problem
        if ( metadata->getCollVersion().epoch() != epoch ) {

            *errMsg = str::stream() << "no need to forget pending chunk "
                                    << "[" << min << "," << max << ")"
                                    << " because the epoch for " << ns << " has changed from "
                                    << epoch << " to " << metadata->getCollVersion().epoch();

            return false;
        }

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );

        CollectionMetadataPtr cloned( metadata->cloneMinusPending( chunk, errMsg ) );
        if ( !cloned ) return false;

        _collMetadata[ns] = cloned;
        return true;
    }

    void ShardingState::splitChunk( const string& ns,
                                    const BSONObj& min,
                                    const BSONObj& max,
                                    const vector<BSONObj>& splitKeys,
                                    ChunkVersion version )
    {
        scoped_lock lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() ) ;

        ChunkType chunk;
        chunk.setMin( min );
        chunk.setMax( max );
        string errMsg;

        CollectionMetadataPtr cloned( it->second->cloneSplit( chunk, splitKeys, version, &errMsg ) );
        // uassert to match old behavior, TODO: report errors w/o throwing
        uassert( 16857, errMsg, NULL != cloned.get() );

        _collMetadata[ns] = cloned;
    }

    void ShardingState::mergeChunks( const string& ns,
                                     const BSONObj& minKey,
                                     const BSONObj& maxKey,
                                     ChunkVersion mergedVersion ) {

        scoped_lock lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        verify( it != _collMetadata.end() );

        string errMsg;

        CollectionMetadataPtr cloned( it->second->cloneMerge( minKey,
                                                              maxKey,
                                                              mergedVersion,
                                                              &errMsg ) );
        // uassert to match old behavior, TODO: report errors w/o throwing
        uassert( 17004, errMsg, NULL != cloned.get() );

        _collMetadata[ns] = cloned;
    }

    void ShardingState::resetMetadata( const string& ns ) {
        scoped_lock lk( _mutex );

        warning() << "resetting metadata for " << ns << ", this should only be used in testing"
                  << endl;

        _collMetadata.erase( ns );
    }

    Status ShardingState::refreshMetadataIfNeeded( const string& ns,
                                                   const ChunkVersion& reqShardVersion,
                                                   ChunkVersion* latestShardVersion )
    {
        // The _configServerTickets serializes this process such that only a small number of threads
        // can try to refresh at the same time.

        LOG( 2 ) << "metadata refresh requested for " << ns << " at shard version "
                 << reqShardVersion << endl;

        //
        // Queuing of refresh requests starts here when remote reload is needed. This may take time.
        // TODO: Explicitly expose the queuing discipline.
        //

        _configServerTickets.waitForTicket();
        TicketHolderReleaser needTicketFrom( &_configServerTickets );

        //
        // Fast path - check if the requested version is at a higher version than the current
        // metadata version or a different epoch before verifying against config server.
        //

        CollectionMetadataPtr storedMetadata;
        {
            scoped_lock lk( _mutex );
            CollectionMetadataMap::iterator it = _collMetadata.find( ns );
            if ( it != _collMetadata.end() ) storedMetadata = it->second;
        }
        ChunkVersion storedShardVersion;
        if ( storedMetadata ) storedShardVersion = storedMetadata->getShardVersion();
        *latestShardVersion = storedShardVersion;

        if ( storedShardVersion >= reqShardVersion &&
             storedShardVersion.epoch() == reqShardVersion.epoch() ) {

            // Don't need to remotely reload if we're in the same epoch with a >= version
            return Status::OK();
        }

        //
        // Slow path - remotely reload
        //
        // Cases:
        // A) Initial config load and/or secondary take-over.
        // B) Migration TO this shard finished, notified by mongos.
        // C) Dropping a collection, notified (currently) by mongos.
        // D) Stale client wants to reload metadata with a different *epoch*, so we aren't sure.

        if ( storedShardVersion.epoch() != reqShardVersion.epoch() ) {
            // Need to remotely reload if our epochs aren't the same, to verify
            LOG( 1 ) << "metadata change requested for " << ns << ", from shard version "
                     << storedShardVersion << " to " << reqShardVersion
                     << ", need to verify with config server" << endl;
        }
        else {
            // Need to remotely reload since our epochs aren't the same but our version is greater
            LOG( 1 ) << "metadata version update requested for " << ns
                     << ", from shard version " << storedShardVersion << " to " << reqShardVersion
                     << ", need to verify with config server" << endl;
        }

        return doRefreshMetadata( ns, reqShardVersion, true, latestShardVersion );
    }

    Status ShardingState::refreshMetadataNow( const string& ns, ChunkVersion* latestShardVersion )
    {
        return doRefreshMetadata( ns, ChunkVersion( 0, 0, OID() ), false, latestShardVersion );
    }

    Status ShardingState::doRefreshMetadata( const string& ns,
                                             const ChunkVersion& reqShardVersion,
                                             bool useRequestedVersion,
                                             ChunkVersion* latestShardVersion )
    {
        // The idea here is that we're going to reload the metadata from the config server, but
        // we need to do so outside any locks.  When we get our result back, if the current metadata
        // has changed, we may not be able to install the new metadata.

        //
        // Get the initial metadata
        // No DBLock is needed since the metadata is expected to change during reload.
        //

        CollectionMetadataPtr beforeMetadata;
        {
            scoped_lock lk( _mutex );
            CollectionMetadataMap::iterator it = _collMetadata.find( ns );
            if ( it != _collMetadata.end() ) beforeMetadata = it->second;
        }

        ChunkVersion beforeShardVersion;
        if ( beforeMetadata ) beforeShardVersion = beforeMetadata->getShardVersion();
        *latestShardVersion = beforeShardVersion;

        //
        // Determine whether we need to diff or fully reload
        //

        bool fullReload = false;
        if ( !beforeMetadata ) {
            // We don't have any metadata to reload from
            fullReload = true;
        }
        else if ( useRequestedVersion && reqShardVersion.epoch() != beforeShardVersion.epoch() ) {
            // It's not useful to use the metadata as a base because we think the epoch will differ
            fullReload = true;
        }

        //
        // Load the metadata from the remote server, start construction
        //

        LOG( 0 ) << "remotely refreshing metadata for " << ns
                 << ( useRequestedVersion ?
                      string( " with requested shard version " ) + reqShardVersion.toString() : "" )
                 << ( fullReload ?
                      ", current shard version is " : " based on current shard version " )
                 << beforeShardVersion << endl;

        string errMsg;
        ConnectionString configServerLoc = ConnectionString::parse( _configServer, errMsg );
        MetadataLoader mdLoader( configServerLoc );
        CollectionMetadata* remoteMetadataRaw = new CollectionMetadata();
        CollectionMetadataPtr remoteMetadata( remoteMetadataRaw );

        Timer refreshTimer;
        Status status =
                mdLoader.makeCollectionMetadata( ns,
                                                 _shardName,
                                                 ( fullReload ? NULL : beforeMetadata.get() ),
                                                 remoteMetadataRaw );
        long long refreshMillis = refreshTimer.millis();

        if ( status.code() == ErrorCodes::NamespaceNotFound ) {
            remoteMetadata.reset();
            remoteMetadataRaw = NULL;
        }
        else if ( !status.isOK() ) {

            warning() << "could not remotely refresh metadata for " << ns
                      << causedBy( status.reason() ) << endl;

            return status;
        }

        ChunkVersion remoteShardVersion;
        if ( remoteMetadata ) remoteShardVersion = remoteMetadata->getShardVersion();

        //
        // Get ready to install loaded metadata if needed
        //

        CollectionMetadataPtr afterMetadata;
        ChunkVersion afterShardVersion;
        ChunkVersion::VersionChoice choice;

        // If we choose to install the new metadata, this describes the kind of install
        enum InstallType {
            InstallType_New, InstallType_Update, InstallType_Replace, InstallType_Drop,
            InstallType_None
        } installType = InstallType_None; // compiler complains otherwise

        {
            // DBLock needed since we're now potentially changing the metadata, and don't want
            // reads/writes to be ongoing.
            Lock::DBWrite writeLk( ns );

            //
            // Get the metadata now that the load has completed
            //

            scoped_lock lk( _mutex );
            CollectionMetadataMap::iterator it = _collMetadata.find( ns );
            if ( it != _collMetadata.end() ) afterMetadata = it->second;

            if ( afterMetadata ) afterShardVersion = afterMetadata->getShardVersion();
            *latestShardVersion = afterShardVersion;

            //
            // Resolve newer pending chunks with the remote metadata, finish construction
            //

            status = mdLoader.promotePendingChunks( afterMetadata.get(), remoteMetadataRaw );

            if ( !status.isOK() ) {

                warning() << "remote metadata for " << ns
                          << " is inconsistent with current pending chunks"
                          << causedBy( status.reason() ) << endl;

                return status;
            }

            //
            // Compare the 'before', 'after', and 'remote' versions/epochs and choose newest
            // Zero-epochs (sentinel value for "dropped" collections), are tested by
            // !epoch.isSet().
            //

            choice = ChunkVersion::chooseNewestVersion( beforeShardVersion,
                                                        afterShardVersion,
                                                        remoteShardVersion );

            if ( choice == ChunkVersion::VersionChoice_Remote ) {

                if ( !afterShardVersion.epoch().isSet() ) {

                    // First metadata load
                    installType = InstallType_New;
                    dassert( it == _collMetadata.end() );
                    _collMetadata.insert( make_pair( ns, remoteMetadata ) );
                }
                else if ( remoteShardVersion.epoch().isSet() &&
                          remoteShardVersion.epoch() == afterShardVersion.epoch() ) {

                    // Update to existing metadata
                    installType = InstallType_Update;

                    // Invariant: If CollMetadata was not found, version should be have been 0.
                    dassert( it != _collMetadata.end() );
                    it->second = remoteMetadata;
                }
                else if ( remoteShardVersion.epoch().isSet() ) {

                    // New epoch detected, replacing metadata
                    installType = InstallType_Replace;

                    // Invariant: If CollMetadata was not found, version should be have been 0.
                    dassert( it != _collMetadata.end() );
                    it->second = remoteMetadata;
                }
                else {
                    dassert( !remoteShardVersion.epoch().isSet() );

                    // Drop detected
                    installType = InstallType_Drop;
                    _collMetadata.erase( it );
                }

                *latestShardVersion = remoteShardVersion;
            }
        }
        // End _mutex
        // End DBWrite

        //
        // Do messaging based on what happened above
        //

        string versionMsg = str::stream()
            << " (loaded version : " << remoteShardVersion.toString()
            << ( beforeShardVersion.epoch() == afterShardVersion.epoch() ?
                     string( ", stored version : " ) + afterShardVersion.toString() :
                     string( ", stored versions : " ) +
                         beforeShardVersion.toString() + " / " + afterShardVersion.toString() )
            << ", took " << refreshMillis << "ms)";

        if ( choice == ChunkVersion::VersionChoice_Unknown ) {

            string errMsg =
                str::stream() << "need to retry loading metadata for " << ns
                              << ", collection may have been dropped or recreated during load"
                              << versionMsg;

            warning() << errMsg << endl;
            return Status( ErrorCodes::RemoteChangeDetected, errMsg );
        }

        if ( choice == ChunkVersion::VersionChoice_Local ) {

            LOG( 0 ) << "newer metadata not found for " << ns << versionMsg << endl;
            return Status::OK();
        }

        dassert( choice == ChunkVersion::VersionChoice_Remote );

        switch( installType ) {
        case InstallType_New:
            LOG( 0 ) << "loaded new metadata for " << ns << versionMsg << endl;
            break;
        case InstallType_Update:
            LOG( 0 ) << "loaded newer metadata for " << ns << versionMsg << endl;
            break;
        case InstallType_Replace:
            LOG( 0 ) << "replacing metadata for " << ns << versionMsg << endl;
            break;
        case InstallType_Drop:
            LOG( 0 ) << "dropping metadata for " << ns << versionMsg << endl;
            break;
        default:
            verify( false );
            break;
        }

        return Status::OK();
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

            for ( CollectionMetadataMap::iterator it = _collMetadata.begin(); it != _collMetadata.end(); ++it ) {
                CollectionMetadataPtr p = it->second;
                bb.appendTimestamp( it->first , p->getShardVersion().toLong() );
            }
            bb.done();
        }

    }

    bool ShardingState::needCollectionMetadata( const string& ns ) const {
        if ( ! _enabled )
            return false;

        if ( ! ShardedConnectionInfo::get( false ) )
            return false;

        return true;
    }

    CollectionMetadataPtr ShardingState::getCollectionMetadata( const string& ns ) {
        scoped_lock lk( _mutex );

        CollectionMetadataMap::const_iterator it = _collMetadata.find( ns );
        if ( it == _collMetadata.end() ) {
            return CollectionMetadataPtr();
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

    const ChunkVersion ShardedConnectionInfo::getVersion( const string& ns ) const {
        NSVersionMap::const_iterator it = _versions.find( ns );
        if ( it != _versions.end() ) {
            return it->second;
        }
        else {
            return ChunkVersion( 0, OID() );
        }
    }

    void ShardedConnectionInfo::setVersion( const string& ns , const ChunkVersion& version ) {
        _versions[ns] = version;
    }

    void ShardedConnectionInfo::addHook() {
        static mongo::mutex lock("ShardedConnectionInfo::addHook mutex");
        static bool done = false;

        scoped_lock lk(lock);
        if (!done) {
            log() << "first cluster operation detected, adding sharding hook to enable versioning "
                    "and authentication to remote servers" << endl;
            pool.addHook(new ShardingConnectionHook(false));
            shardConnectionPool.addHook(new ShardingConnectionHook(true));
            done = true;
        }
    }

    void ShardedConnectionInfo::setID( const OID& id ) {
        _id = id;
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

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::unsetSharding);
            out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
        }

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
        
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::setShardVersion);
            out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
        }

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
                                                  
                errmsg = str::stream() << "mongos specified a different config database string : "
                                       << "stored : " << shardingState.getConfigServer()
                                       << " vs given : " << configdb;
                return false;
            }
            
            if ( ! authoritative ) {
                result.appendBool( "need_authoritative" , true );
                errmsg = "first setShardVersion";
                return false;
            }
            
            if ( locked ) {
                ShardingState::initialize(configdb);
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

            if( ! ChunkVersion::canParseBSON( cmdObj, "version" ) ){
                errmsg = "need to specify version";
                return false;
            }

            const ChunkVersion version = ChunkVersion::fromBSON( cmdObj, "version" );
            
            // step 3

            const ChunkVersion oldVersion = info->getVersion(ns);
            const ChunkVersion globalVersion = shardingState.getVersion(ns);

            oldVersion.addToBSON( result, "oldVersion" );
            
            if ( globalVersion.isSet() && version.isSet() ) {
                // this means there is no reset going on an either side
                // so its safe to make some assumptions

                if ( version.isWriteCompatibleWith( globalVersion ) ) {
                    // mongos and mongod agree!
                    if ( ! oldVersion.isWriteCompatibleWith( version ) ) {
                        if ( oldVersion < globalVersion &&
                             oldVersion.hasCompatibleEpoch(globalVersion) )
                        {
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
        
            if ( oldVersion.isSet() && ! globalVersion.isSet() ) {
                // this had been reset
                info->setVersion( ns , ChunkVersion( 0, OID() ) );
            }

            if ( ! version.isSet() && ! globalVersion.isSet() ) {
                // this connection is cleaning itself
                info->setVersion( ns , ChunkVersion( 0, OID() ) );
                return true;
            }

            // Cases below all either return OR fall-through to remote metadata reload.
            if ( version.isSet() || !globalVersion.isSet() ) {

                // Not Dropping

                // TODO: Refactor all of this
                if ( version < oldVersion && version.hasCompatibleEpoch( oldVersion ) ) {
                    errmsg = "this connection already had a newer version of collection '" + ns + "'";
                    result.append( "ns" , ns );
                    version.addToBSON( result, "newVersion" );
                    globalVersion.addToBSON( result, "globalVersion" );
                    return false;
                }

                // TODO: Refactor all of this
                if ( version < globalVersion && version.hasCompatibleEpoch( globalVersion ) ) {
                    while ( shardingState.inCriticalMigrateSection() ) {
                        log() << "waiting till out of critical section" << endl;
                        shardingState.waitTillNotInCriticalSection( 10 );
                    }
                    errmsg = "shard global version for collection is higher than trying to set to '" + ns + "'";
                    result.append( "ns" , ns );
                    version.addToBSON( result, "version" );
                    globalVersion.addToBSON( result, "globalVersion" );
                    result.appendBool( "reloadConfig" , true );
                    return false;
                }

                if ( ! globalVersion.isSet() && ! authoritative ) {
                    // Needed b/c when the last chunk is moved off a shard, the version gets reset to zero, which
                    // should require a reload.
                    while ( shardingState.inCriticalMigrateSection() ) {
                        log() << "waiting till out of critical section" << endl;
                        shardingState.waitTillNotInCriticalSection( 10 );
                    }

                    // need authoritative for first look
                    result.append( "ns" , ns );
                    result.appendBool( "need_authoritative" , true );
                    errmsg = "first time for collection '" + ns + "'";
                    return false;
                }

                // Fall through to metadata reload below
            }
            else {

                // Dropping

                if ( ! authoritative ) {
                    result.appendBool( "need_authoritative" , true );
                    result.append( "ns" , ns );
                    globalVersion.addToBSON( result, "globalVersion" );
                    errmsg = "dropping needs to be authoritative";
                    return false;
                }

                // Fall through to metadata reload below
            }

            ChunkVersion currVersion;
            Status status = shardingState.refreshMetadataIfNeeded( ns, version, &currVersion );

            if (!status.isOK()) {

                // The reload itself was interrupted or confused here

                errmsg = str::stream() << "could not refresh metadata for " << ns
                                       << " with requested shard version " << version.toString()
                                       << ", stored shard version is " << currVersion.toString()
                                       << causedBy( status.reason() );

                warning() << errmsg << endl;

                result.append( "ns" , ns );
                version.addToBSON( result, "version" );
                currVersion.addToBSON( result, "globalVersion" );
                result.appendBool( "reloadConfig", true );

                return false;
            }
            else if ( !version.isWriteCompatibleWith( currVersion ) ) {

                // We reloaded a version that doesn't match the version mongos was trying to
                // set.

                errmsg = str::stream() << "requested shard version differs from"
                                       << " config shard version for " << ns
                                       << ", requested version is " << version.toString()
                                       << " but found version " << currVersion.toString();

                OCCASIONALLY warning() << errmsg << endl;

                // WARNING: the exact fields below are important for compatibility with mongos
                // version reload.

                result.append( "ns" , ns );
                currVersion.addToBSON( result, "globalVersion" );

                // If this was a reset of a collection or the last chunk moved out, inform mongos to
                // do a full reload.
                if (currVersion.epoch() != version.epoch() || !currVersion.isSet() ) {
                    result.appendBool( "reloadConfig", true );
                    // Zero-version also needed to trigger full mongos reload, sadly
                    // TODO: Make this saner, and less impactful (full reload on last chunk is bad)
                    ChunkVersion( 0, 0, OID() ).addToBSON( result, "version" );
                    // For debugging
                    version.addToBSON( result, "origVersion" );
                }
                else {
                    version.addToBSON( result, "version" );
                }

                return false;
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

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getShardVersion);
            out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
        }

        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string ns = cmdObj["getShardVersion"].valuestrsafe();
            if ( ns.size() == 0 ) {
                errmsg = "need to specify full namespace";
                return false;
            }

            result.append( "configServer" , shardingState.getConfigServer() );

            result.appendTimestamp( "global" , shardingState.getVersion(ns).toLong() );

            ShardedConnectionInfo* info = ShardedConnectionInfo::get( false );
            result.appendBool( "inShardedMode" , info != 0 );
            if ( info )
                result.appendTimestamp( "mine" , info->getVersion(ns).toLong() );
            else
                result.appendTimestamp( "mine" , 0 );

            if ( cmdObj["fullMetadata"].trueValue() ) {
                CollectionMetadataPtr metadata = shardingState.getCollectionMetadata( ns );
                if ( metadata ) result.append( "metadata", metadata->toBSON() );
                else result.append( "metadata", BSONObj() );
            }

            return true;
        }

    } getShardVersion;

    class ShardingStateCmd : public MongodShardCommand {
    public:
        ShardingStateCmd() : MongodShardCommand( "shardingState" ) {}

        virtual LockType locktype() const { return WRITE; } // TODO: figure out how to make this not need to lock

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::shardingState);
            out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
        }

        bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            shardingState.appendInfo( result );
            return true;
        }

    } shardingStateCmd;

    /**
     * @ return true if not in sharded mode
                     or if version for this client is ok
     */
    bool shardVersionOk( const string& ns , string& errmsg, ChunkVersion& received, ChunkVersion& wanted ) {

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

        // TODO : all collections at some point, be sharded or not, will have a version
        //  (and a CollectionMetadata)
        received = info->getVersion( ns );
        wanted = shardingState.getVersion( ns );

        if( received.isWriteCompatibleWith( wanted ) ) return true;

        //
        // Figure out exactly why not compatible, send appropriate error message
        // The versions themselves are returned in the error, so not needed in messages here
        //

        // Check epoch first, to send more meaningful message, since other parameters probably
        // won't match either
        if( ! wanted.hasCompatibleEpoch( received ) ){
            errmsg = str::stream() << "version epoch mismatch detected for " << ns << ", "
                                   << "the collection may have been dropped and recreated";
            return false;
        }

        if( ! wanted.isSet() && received.isSet() ){
            errmsg = str::stream() << "this shard no longer contains chunks for " << ns << ", "
                                   << "the collection may have been dropped";
            return false;
        }

        if( wanted.isSet() && ! received.isSet() ){
            errmsg = str::stream() << "this shard contains versioned chunks for " << ns << ", "
                                   << "but no version set in request";
            return false;
        }

        if( wanted.majorVersion() != received.majorVersion() ){

            //
            // Could be > or < - wanted is > if this is the source of a migration,
            // wanted < if this is the target of a migration
            //

            errmsg = str::stream() << "version mismatch detected for " << ns << ", "
                                   << "stored major version " << wanted.majorVersion()
                                   << " does not match received " << received.majorVersion();
            return false;
        }

        // Those are all the reasons the versions can mismatch
        verify( false );

        return false;

    }

    void usingAShardConnection( const string& addr ) {
    }

}
