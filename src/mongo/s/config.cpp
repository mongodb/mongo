/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/config.h"

#include <boost/scoped_ptr.hpp>
#include <pcrecpp.h>

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/client.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/server_options.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_settings.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/server.h"
#include "mongo/s/type_locks.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_tags.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::pair;
    using std::set;
    using std::stringstream;
    using std::vector;

    int ConfigServer::VERSION = 3;
    Shard Shard::EMPTY;


    CollectionInfo::CollectionInfo(const CollectionType& coll) {
        _dropped = coll.getDropped();

        shard(new ChunkManager(coll));
        _dirty = false;
    }

    CollectionInfo::~CollectionInfo() {

    }

    void CollectionInfo::resetCM(ChunkManager* cm) {
        invariant(cm);
        invariant(_cm);

        _cm.reset(cm);
    }
    
    void CollectionInfo::shard(ChunkManager* manager) {
        // Do this *first* so we're invisible to everyone else
        manager->loadExistingRanges(nullptr);

        //
        // Collections with no chunks are unsharded, no matter what the collections entry says
        // This helps prevent errors when dropping in a different process
        //

        if (manager->numChunks() != 0) {
            useChunkManager(ChunkManagerPtr(manager));
        }
        else{
            warning() << "no chunks found for collection " << manager->getns()
                      << ", assuming unsharded";
            unshard();
        }
    }

    void CollectionInfo::unshard() {
        _cm.reset();
        _dropped = true;
        _dirty = true;
        _key = BSONObj();
    }

    void CollectionInfo::useChunkManager(ChunkManagerPtr manager) {
        _cm = manager;
        _key = manager->getShardKeyPattern().toBSON().getOwned();
        _unique = manager->isUnique();
        _dirty = true;
        _dropped = false;
    }

    void CollectionInfo::save(const string& ns) {
        CollectionType coll;
        coll.setNs(NamespaceString{ns});

        if (_cm) {
            invariant(!_dropped);
            coll.setEpoch(_cm->getVersion().epoch());
            // TODO(schwerin): The following isn't really a date, but is stored as one in-memory and
            // in config.collections, as a historical oddity.
            coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(_cm->getVersion().toLong()));
            coll.setKeyPattern(_cm->getShardKeyPattern().toBSON());
            coll.setUnique(_cm->isUnique());
        }
        else {
            invariant(_dropped);
            coll.setDropped(true);
            coll.setEpoch(ChunkVersion::DROPPED().epoch());
            coll.setUpdatedAt(Date_t::now());
        }

        uassertStatusOK(grid.catalogManager()->updateCollection(ns, coll));
        _dirty = false;
    }

    DBConfig::DBConfig(std::string name, const DatabaseType& dbt)
        : _name(name) {

        invariant(_name == dbt.getName());
        _primary.reset(dbt.getPrimary());
        _shardingEnabled = dbt.getSharded();
    }

    bool DBConfig::isSharded( const string& ns ) {
        if ( ! _shardingEnabled )
            return false;
        boost::lock_guard<boost::mutex> lk( _lock );
        return _isSharded( ns );
    }

    bool DBConfig::_isSharded( const string& ns ) {
        if (!_shardingEnabled) {
            return false;
        }

        CollectionInfoMap::iterator i = _collections.find( ns );
        if (i == _collections.end()) {
            return false;
        }

        return i->second.isSharded();
    }

    const Shard& DBConfig::getShard( const string& ns ) {
        if ( isSharded( ns ) )
            return Shard::EMPTY;

        uassert( 10178 ,  "no primary!" , _primary.ok() );
        return _primary;
    }

    void DBConfig::enableSharding( bool save ) {
        if ( _shardingEnabled )
            return;
        
        verify( _name != "config" );

        boost::lock_guard<boost::mutex> lk( _lock );
        _shardingEnabled = true;
        if( save ) _save();
    }

    bool DBConfig::removeSharding( const string& ns ) {
        if ( ! _shardingEnabled ) {

            warning() << "could not remove sharding for collection " << ns
                      << ", sharding not enabled for db" << endl;
            return false;
        }

        boost::lock_guard<boost::mutex> lk( _lock );

        CollectionInfoMap::iterator i = _collections.find( ns );

        if ( i == _collections.end() )
            return false;

        CollectionInfo& ci = _collections[ns];
        if ( ! ci.isSharded() ){

            warning() << "could not remove sharding for collection " << ns
                      << ", no sharding information found" << endl;
            return false;
        }

        ci.unshard();
        _save( false, true );
        return true;
    }

    // Handles weird logic related to getting *either* a chunk manager *or* the collection primary shard
    void DBConfig::getChunkManagerOrPrimary(const string& ns,
                                            boost::shared_ptr<ChunkManager>& manager,
                                            boost::shared_ptr<Shard>& primary) {

        // The logic here is basically that at any time, our collection can become sharded or unsharded
        // via a command.  If we're not sharded, we want to send data to the primary, if sharded, we want
        // to send data to the correct chunks, and we can't check both w/o the lock.

        manager.reset();
        primary.reset();

        {
            boost::lock_guard<boost::mutex> lk( _lock );

            CollectionInfoMap::iterator i = _collections.find( ns );

            // No namespace
            if( i == _collections.end() ){
                // If we don't know about this namespace, it's unsharded by default
                primary.reset( new Shard( _primary ) );
            }
            else {
                CollectionInfo& cInfo = i->second;

                // TODO: we need to be careful about handling shardingEnabled, b/c in some places we seem to use and
                // some we don't.  If we use this function in combination with just getChunkManager() on a slightly
                // borked config db, we'll get lots of staleconfig retries
                if( _shardingEnabled && cInfo.isSharded() ){
                    manager = cInfo.getCM();
                }
                else{
                    // Make a copy, we don't want to be tied to this config object
                    primary.reset( new Shard( _primary ) );
                }
            }
        }

        verify( manager || primary );
        verify( ! manager || ! primary );

    }


    ChunkManagerPtr DBConfig::getChunkManagerIfExists( const string& ns, bool shouldReload, bool forceReload ){
	
        // Don't report exceptions here as errors in GetLastError
        LastError::Disabled ignoreForGLE(&LastError::get(cc()));
         
        try{
            return getChunkManager( ns, shouldReload, forceReload );
        }
        catch( AssertionException& e ){
            warning() << "chunk manager not found for " << ns << causedBy( e ) << endl;
            return ChunkManagerPtr();
        }
    }

    boost::shared_ptr<ChunkManager> DBConfig::getChunkManager(const string& ns,
                                                              bool shouldReload,
                                                              bool forceReload) {
        BSONObj key;
        ChunkVersion oldVersion;
        ChunkManagerPtr oldManager;

        {
            boost::lock_guard<boost::mutex> lk( _lock );
            
            bool earlyReload = ! _collections[ns].isSharded() && ( shouldReload || forceReload );
            if ( earlyReload ) {
                // this is to catch cases where there this is a new sharded collection
                _reload();
            }

            CollectionInfo& ci = _collections[ns];
            uassert( 10181 ,  (string)"not sharded:" + ns , ci.isSharded() );
            verify( ! ci.key().isEmpty() );
            
            if ( ! ( shouldReload || forceReload ) || earlyReload )
                return ci.getCM();

            key = ci.key().copy();
            if ( ci.getCM() ){
                oldManager = ci.getCM();
                oldVersion = ci.getCM()->getVersion();
            }
        }
        
        verify( ! key.isEmpty() );
        
        // TODO: We need to keep this first one-chunk check in until we have a more efficient way of
        // creating/reusing a chunk manager, as doing so requires copying the full set of chunks currently
        std::vector<ChunkType> newestChunk;
        if ( oldVersion.isSet() && ! forceReload ) {
            uassertStatusOK(grid.catalogManager()->getChunks(
                                        Query(BSON(ChunkType::ns(ns)))
                                            .sort(ChunkType::DEPRECATED_lastmod(), -1),
                                        1,
                                        &newestChunk));

            if (!newestChunk.empty()) {
                invariant(newestChunk.size() == 1);
                ChunkVersion v = newestChunk[0].getVersion();
                if (v.equals(oldVersion)) {
                    boost::lock_guard<boost::mutex> lk( _lock );
                    const CollectionInfo& ci = _collections[ns];
                    uassert(15885,
                            str::stream() << "not sharded after reloading from chunks : "
                                          << ns, ci.isSharded());
                    return ci.getCM();
                }
            }

        }
        else if (!oldVersion.isSet()) {
            warning() << "version 0 found when " << (forceReload ? "reloading" : "checking")
                      << " chunk manager; collection '" << ns << "' initially detected as sharded";
        }

        // we are not locked now, and want to load a new ChunkManager
        
        auto_ptr<ChunkManager> temp;

        {
            boost::lock_guard<boost::mutex> lll ( _hitConfigServerLock );
            
            if (!newestChunk.empty() && !forceReload) {
                // if we have a target we're going for
                // see if we've hit already
                
                boost::lock_guard<boost::mutex> lk( _lock );
                CollectionInfo& ci = _collections[ns];
                if ( ci.isSharded() && ci.getCM() ) {

                    ChunkVersion currentVersion = newestChunk[0].getVersion();

                    // Only reload if the version we found is newer than our own in the same
                    // epoch
                    if (currentVersion <= ci.getCM()->getVersion() &&
                            ci.getCM()->getVersion().hasEqualEpoch(currentVersion)) {
                        return ci.getCM();
                    }
                }
            }
            
            temp.reset(new ChunkManager(oldManager->getns(),
                                        oldManager->getShardKeyPattern(),
                                        oldManager->isUnique()));
            temp->loadExistingRanges(oldManager.get());

            if ( temp->numChunks() == 0 ) {
                // maybe we're not sharded any more
                reload(); // this is a full reload
                return getChunkManager( ns , false );
            }
        }

        boost::lock_guard<boost::mutex> lk( _lock );
        
        CollectionInfo& ci = _collections[ns];
        uassert( 14822 ,  (string)"state changed in the middle: " + ns , ci.isSharded() );

        // Reset if our versions aren't the same
        bool shouldReset = ! temp->getVersion().equals( ci.getCM()->getVersion() );
        
        // Also reset if we're forced to do so
        if( ! shouldReset && forceReload ){
            shouldReset = true;
            warning() << "chunk manager reload forced for collection '" << ns
                      << "', config version is " << temp->getVersion() << endl;
        }

        //
        // LEGACY BEHAVIOR
        // It's possible to get into a state when dropping collections when our new version is less than our prev
        // version.  Behave identically to legacy mongos, for now, and warn to draw attention to the problem.
        // TODO: Assert in next version, to allow smooth upgrades
        //

        if( shouldReset && temp->getVersion() < ci.getCM()->getVersion() ){
            shouldReset = false;
            warning() << "not resetting chunk manager for collection '" << ns
                      << "', config version is " << temp->getVersion() << " and "
                      << "old version is " << ci.getCM()->getVersion() << endl;
        }

        // end legacy behavior

        if ( shouldReset ){
            ci.resetCM( temp.release() );
        }
        
        uassert( 15883 , str::stream() << "not sharded after chunk manager reset : " << ns , ci.isSharded() );
        return ci.getCM();
    }

    void DBConfig::setPrimary( const std::string& s ) {
        boost::lock_guard<boost::mutex> lk( _lock );
        _primary.reset( s );
        _save();
    }

    bool DBConfig::load() {
        boost::lock_guard<boost::mutex> lk( _lock );
        return _load();
    }

    bool DBConfig::_load() {
        StatusWith<DatabaseType> status = grid.catalogManager()->getDatabase(_name);
        if (status == ErrorCodes::DatabaseNotFound) {
            return false;
        }

        // All other errors are connectivity, etc so throw an exception.
        uassertStatusOK(status.getStatus());

        DatabaseType dbt = status.getValue();
        invariant(_name == dbt.getName());
        _primary.reset(dbt.getPrimary());
        _shardingEnabled = dbt.getSharded();

        // Load all collections
        vector<CollectionType> collections;
        uassertStatusOK(grid.catalogManager()->getCollections(&_name, &collections));

        int numCollsErased = 0;
        int numCollsSharded = 0;

        for (const auto& coll : collections) {
            if (coll.getDropped()) {
                _collections.erase(coll.getNs());
                numCollsErased++;
            }
            else {
                _collections[coll.getNs()] = CollectionInfo(coll);
                numCollsSharded++;
            }
        }

        LOG(2) << "found " << numCollsSharded << " collections left and "
                           << numCollsErased << " collections dropped for database " << _name;

        return true;
    }

    void DBConfig::_save(bool db, bool coll) {
        if (db) {
            DatabaseType dbt;
            dbt.setName(_name);
            dbt.setPrimary(_primary.getName());
            dbt.setSharded(_shardingEnabled);

            uassertStatusOK(grid.catalogManager()->updateDatabase(_name, dbt));
        }

        if (coll) {
            for (CollectionInfoMap::iterator i = _collections.begin();
                 i != _collections.end();
                 ++i) {

                if (!i->second.isDirty()) {
                    continue;
                }

                i->second.save(i->first);
            }
        }
    }

    bool DBConfig::reload() {
        bool successful = false;

        {
            boost::lock_guard<boost::mutex> lk( _lock );
            successful = _reload();
        }

        // If we aren't successful loading the database entry, we don't want to keep the stale
        // object around which has invalid data.
        if (!successful) {
            grid.catalogCache()->invalidate(_name);
        }

        return successful;
    }

    bool DBConfig::_reload() {
        // TODO: i don't think is 100% correct
        return _load();
    }

    bool DBConfig::dropDatabase(string& errmsg) {
        /**
         * 1) update config server
         * 2) drop and reset sharded collections
         * 3) drop and reset primary
         * 4) drop everywhere to clean up loose ends
         */

        log() << "DBConfig::dropDatabase: " << _name << endl;
        grid.catalogManager()->logChange(NULL, "dropDatabase.start", _name, BSONObj());

        // 1
        grid.catalogCache()->invalidate(_name);

        Status result = grid.catalogManager()->remove(DatabaseType::ConfigNS,
                                                      BSON(DatabaseType::name(_name)),
                                                      0,
                                                      NULL);
        if (!result.isOK()) {
            errmsg = result.reason();
            log() << "could not drop '" << _name << "': " << errmsg << endl;
            return false;
        }

        LOG(1) << "\t removed entry from config server for: " << _name << endl;

        set<Shard> allServers;

        // 2
        while ( true ) {
            int num = 0;
            if (!_dropShardedCollections(num, allServers, errmsg)) {
                return 0;
            }

            log() << "   DBConfig::dropDatabase: " << _name
                  << " dropped sharded collections: " << num;

            if (num == 0) {
                break;
            }
        }

        // 3
        {
            ScopedDbConnection conn(_primary.getConnString(), 30.0);
            BSONObj res;
                if ( ! conn->dropDatabase( _name , &res ) ) {
                errmsg = res.toString();
                return 0;
            }
            conn.done();
        }

        // 4
        for ( set<Shard>::iterator i=allServers.begin(); i!=allServers.end(); i++ ) {
            ScopedDbConnection conn(i->getConnString(), 30.0);
            BSONObj res;
            if ( ! conn->dropDatabase( _name , &res ) ) {
                errmsg = res.toString();
                return 0;
            }
            conn.done();
        }

        LOG(1) << "\t dropped primary db for: " << _name << endl;

        grid.catalogManager()->logChange(NULL, "dropDatabase", _name, BSONObj());

        return true;
    }

    bool DBConfig::_dropShardedCollections( int& num, set<Shard>& allServers , string& errmsg ) {
        num = 0;
        set<string> seen;
        while ( true ) {
            CollectionInfoMap::iterator i = _collections.begin();
            for ( ; i != _collections.end(); ++i ) {
                // log() << "coll : " << i->first << " and " << i->second.isSharded() << endl;
                if ( i->second.isSharded() )
                    break;
            }

            if ( i == _collections.end() )
                break;

            if ( seen.count( i->first ) ) {
                errmsg = "seen a collection twice!";
                return false;
            }

            seen.insert( i->first );
            LOG(1) << "\t dropping sharded collection: " << i->first << endl;

            i->second.getCM()->getAllShards( allServers );

            uassertStatusOK(grid.catalogManager()->dropCollection(i->first));

            // We should warn, but it's not a fatal error if someone else reloaded the db/coll as
            // unsharded in the meantime
            if( ! removeSharding( i->first ) ){
                warning() << "collection " << i->first
                          << " was reloaded as unsharded before drop completed"
                          << " during drop of all collections" << endl;
            }

            num++;
            uassert( 10184 ,  "_dropShardedCollections too many collections - bailing" , num < 100000 );
            LOG(2) << "\t\t dropped " << num << " so far" << endl;
        }

        return true;
    }

    void DBConfig::getAllShards(set<Shard>& shards) {
        boost::lock_guard<boost::mutex> lk(_lock);
        shards.insert(getPrimary());
        for (CollectionInfoMap::const_iterator it(_collections.begin()), end(_collections.end()); it != end; ++it) {
            if (it->second.isSharded()) {
                it->second.getCM()->getAllShards(shards);
            } // TODO: handle collections on non-primary shard
        }
    }

    void DBConfig::getAllShardedCollections( set<string>& namespaces ) {
        boost::lock_guard<boost::mutex> lk(_lock);

        for( CollectionInfoMap::const_iterator i = _collections.begin(); i != _collections.end(); i++ ) {
            log() << "Coll : " << i->first << " sharded? " << i->second.isSharded() << endl;
            if( i->second.isSharded() ) namespaces.insert( i->first );
        }

    }

    /* --- ConfigServer ---- */

    const std::string& ConfigServer::modelServer() const {
        uassert(10190, "ConfigServer not setup", _primary.ok());
        return _primary.getConnString();
    }

    ConnectionString ConfigServer::getConnectionString() const {
        return _primary.getAddress();
    }

    bool ConfigServer::init( const ConnectionString& configCS ) {
        invariant(configCS.isValid());

        std::vector<HostAndPort> configHostAndPorts = configCS.getServers();
        uassert( 10187 ,  "need configdbs" , configHostAndPorts.size() );

        std::vector<std::string> configHosts;
        set<string> hosts;
        for ( size_t i=0; i<configHostAndPorts.size(); i++ ) {
            string host = configHostAndPorts[i].toString();
            hosts.insert( getHost( host , false ) );
            configHosts.push_back(getHost( host , true ));
        }

        for ( set<string>::iterator i=hosts.begin(); i!=hosts.end(); i++ ) {

            string host = *i;

            // If this is a CUSTOM connection string (for testing) don't do DNS resolution
            string errMsg;
            if ( ConnectionString::parse( host, errMsg ).type() == ConnectionString::CUSTOM ) {
                continue;
            }

            bool ok = false;
            for ( int x=10; x>0; x-- ) {
                if ( ! hostbyname( host.c_str() ).empty() ) {
                    ok = true;
                    break;
                }
                log() << "can't resolve DNS for [" << host << "]  sleeping and trying " << x << " more times" << endl;
                sleepsecs( 10 );
            }
            if ( ! ok )
                return false;
        }

        _config = configHosts;

        string errmsg;
        if( ! checkHostsAreUnique(configHosts, &errmsg) ) {
            error() << errmsg << endl;;
            return false;
        }

        // This should be the first time we are trying to set up the primary shard (i.e. init
        // should be called only once)
        invariant(_primary == Shard::EMPTY);
        _primary = Shard("config", configCS, 0, false);

        Shard::installShard("config", _primary);

        LOG(1) << " config string : " << configCS.toString();

        return true;
    }

    bool ConfigServer::checkHostsAreUnique( const vector<string>& configHosts, string* errmsg ) {

        //If we have one host, its always unique
        if ( configHosts.size() == 1 ) {
            return true;
        }

        //Compare each host with all other hosts.
        set<string> hostsTest;
        pair<set<string>::iterator,bool> ret;
        for ( size_t x=0; x < configHosts.size(); x++) {
            ret = hostsTest.insert( configHosts[x] );
            if ( ret.second == false ) {
               *errmsg = str::stream() << "config servers " << configHosts[x]
                                       << " exists twice in config listing.";
               return false;
            }
        }
        return true;
    }

    int ConfigServer::dbConfigVersion() {
        ScopedDbConnection conn(_primary.getConnString(), 30.0);
        int version = dbConfigVersion( conn.conn() );
        conn.done();
        return version;
    }

    int ConfigServer::dbConfigVersion( DBClientBase& conn ) {
        auto_ptr<DBClientCursor> c = conn.query( "config.version" , BSONObj() );
        int version = 0;
        if ( c->more() ) {
            BSONObj o = c->next();
            version = o["version"].numberInt();
            uassert( 10189 ,  "should only have 1 thing in config.version" , ! c->more() );
        }
        else {
            if (grid.catalogManager()->doShardsExist() ||
                conn.count(DatabaseType::ConfigNS)) {
                version = 1;
            }
        }

        return version;
    }

    void ConfigServer::reloadSettings() {
        set<string> got;

        try {
            ScopedDbConnection conn(_primary.getConnString(), 30.0);
            auto_ptr<DBClientCursor> cursor = conn->query(SettingsType::ConfigNS, BSONObj());
            verify(cursor.get());

            while (cursor->more()) {
                StatusWith<SettingsType> settingsResult =
                    SettingsType::fromBSON(cursor->nextSafe());
                if (!settingsResult.isOK()) {
                    warning() << settingsResult.getStatus();
                    continue;
                }
                SettingsType settings = settingsResult.getValue();
                string key = settings.getKey();
                got.insert(key);

                if (key == SettingsType::ChunkSizeDocKey) {
                    int csize = settings.getChunkSize();

                    // validate chunksize before proceeding
                    if (csize == 0) {
                        // setting was not modified; mark as such
                        got.erase(key);
                        log() << "warning: invalid chunksize (" << csize << ") ignored" << endl;
                    } else {
                        LOG(1) << "MaxChunkSize: " << csize << endl;
                        if (!Chunk::setMaxChunkSizeSizeMB(csize)) {
                            warning() << "invalid chunksize: " << csize << endl;
                        }
                    }
                }
                else if (key == SettingsType::BalancerDocKey) {
                    // ones we ignore here
                }
                else {
                    log() << "warning: unknown setting [" << key << "]";
                }
            }

            conn.done();
        }
        catch (const DBException& ex) {
            warning() << "couldn't load settings on config db" << causedBy(ex);
        }

        if (!got.count(SettingsType::ChunkSizeDocKey)) {
            const int chunkSize = Chunk::MaxChunkSize / (1024 * 1024);
            Status result =
                grid.catalogManager()->insert(SettingsType::ConfigNS,
                                              BSON(SettingsType::key(SettingsType::ChunkSizeDocKey)
                                                   << SettingsType::chunkSize(chunkSize)),
                                              NULL);
            if (!result.isOK()) {
                warning() << "couldn't set chunkSize on config db" << causedBy(result);
            }
        }

        // indexes
        Status result = clusterCreateIndex( ChunkType::ConfigNS,
                                            BSON( ChunkType::ns() << 1 << ChunkType::min() << 1 ),
                                            true, // unique
                                            NULL );

        if (!result.isOK()) {
            warning() << "couldn't create ns_1_min_1 index on config db" << causedBy(result);
        }

        result = clusterCreateIndex( ChunkType::ConfigNS,
                                     BSON( ChunkType::ns() << 1 <<
                                           ChunkType::shard() << 1 <<
                                           ChunkType::min() << 1 ),
                                     true, // unique
                                     NULL );

        if (!result.isOK()) {
            warning() << "couldn't create ns_1_shard_1_min_1 index on config db"
                      << causedBy(result);
        }

        result = clusterCreateIndex( ChunkType::ConfigNS,
                                     BSON( ChunkType::ns() << 1 <<
                                           ChunkType::DEPRECATED_lastmod() << 1 ),
                                     true, // unique
                                     NULL );

        if (!result.isOK()) {
            warning() << "couldn't create ns_1_lastmod_1 index on config db" << causedBy(result);
        }

        result = clusterCreateIndex( ShardType::ConfigNS,
                                     BSON( ShardType::host() << 1 ),
                                     true, // unique
                                     NULL );

        if (!result.isOK()) {
            warning() << "couldn't create host_1 index on config db" << causedBy(result);
        }

        result = clusterCreateIndex(LocksType::ConfigNS,
                                    BSON(LocksType::lockID() << 1),
                                    false, // unique
                                    NULL);

        if (!result.isOK()) {
            warning() << "couldn't create lock id index on config db" << causedBy(result);
        }

        result = clusterCreateIndex( LocksType::ConfigNS,
                                     BSON( LocksType::state() << 1 <<
                                           LocksType::process() << 1 ),
                                     false, // unique
                                     NULL );

        if (!result.isOK()) {
            warning() << "couldn't create state and process id index on config db"
                      << causedBy(result);
        }

        result = clusterCreateIndex( LockpingsType::ConfigNS,
                                     BSON( LockpingsType::ping() << 1 ),
                                     false, // unique
                                     NULL );

        if (!result.isOK()) {
            warning() << "couldn't create lockping ping time index on config db"
                      << causedBy(result);
        }

        result = clusterCreateIndex(TagsType::ConfigNS,
                                    BSON(TagsType::ns() << 1 << TagsType::min() << 1),
                                    true, // unique
                                    NULL);

        if (!result.isOK()) {
            warning() << "could not create index ns_1_min_1: " << causedBy(result);
        }
    }

    string ConfigServer::getHost( const std::string& name , bool withPort ) {
        if ( name.find( ":" ) != string::npos ) {
            if ( withPort )
                return name;
            return name.substr( 0 , name.find( ":" ) );
        }

        if ( withPort ) {
            stringstream ss;
            ss << name << ":" << ServerGlobalParams::ConfigServerPort;
            return ss.str();
        }

        return name;
    }

    void ConfigServer::replicaSetChange(const string& setName, const string& newConnectionString) {
        // This is run in it's own thread. Exceptions escaping would result in a call to terminate.
        Client::initThread("replSetChange");
        try {
            Shard s = Shard::lookupRSName(setName);
            if (s == Shard::EMPTY) {
                LOG(1) << "shard not found for set: " << newConnectionString;
                return;
            }

            Status result = grid.catalogManager()->update(
                                                    ShardType::ConfigNS,
                                                    BSON(ShardType::name(s.getName())),
                                                    BSON("$set" << BSON(ShardType::host(
                                                                            newConnectionString))),
                                                    false, // upsert
                                                    false, // multi
                                                    NULL);
            if (!result.isOK()) {
                error() << "RSChangeWatcher: could not update config db for set: "
                        << setName
                        << " to: " << newConnectionString
                        << ": " << result.reason();
            }
        }
        catch (const std::exception& e) {
            log() << "caught exception while updating config servers: " << e.what();
        }
        catch (...) {
            log() << "caught unknown exception while updating config servers";
        }
    }


    ConfigServer& configServer = *(new ConfigServer());
}
