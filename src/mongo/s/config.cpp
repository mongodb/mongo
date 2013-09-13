// config.cpp

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

#include "mongo/pch.h"

#include "pcrecpp.h"

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/pdfile.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/server.h"
#include "mongo/s/type_changelog.h"
#include "mongo/s/type_chunk.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_database.h"
#include "mongo/s/type_settings.h"
#include "mongo/s/type_shard.h"
#include "mongo/util/net/message.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    int ConfigServer::VERSION = 3;
    Shard Shard::EMPTY;

    OID serverID;

    /* --- DBConfig --- */

    DBConfig::CollectionInfo::CollectionInfo( const BSONObj& in ) {
        _dirty = false;
        _dropped = in[CollectionType::dropped()].trueValue();

        if ( in[CollectionType::keyPattern()].isABSONObj() ) {
            shard( new ChunkManager( in ) );
        }

        _dirty = false;
    }
    
    void DBConfig::CollectionInfo::shard( ChunkManager* manager ){

        // Do this *first* so we're invisible to everyone else
        manager->loadExistingRanges( configServer.getPrimary().getConnString() );

        //
        // Collections with no chunks are unsharded, no matter what the collections entry says
        // This helps prevent errors when dropping in a different process
        //

        if( manager->numChunks() != 0 ){
            _cm = ChunkManagerPtr( manager );
            _key = manager->getShardKey().key().getOwned();
            _unqiue = manager->isUnique();
            _dirty = true;
            _dropped = false;
        }
        else{
            warning() << "no chunks found for collection " << manager->getns()
                      << ", assuming unsharded" << endl;
            unshard();
        }
    }

    void DBConfig::CollectionInfo::unshard() {
        _cm.reset();
        _dropped = true;
        _dirty = true;
        _key = BSONObj();
    }

    void DBConfig::CollectionInfo::save( const string& ns , DBClientBase* conn ) {
        BSONObj key = BSON( "_id" << ns );

        BSONObjBuilder val;
        val.append(CollectionType::ns(), ns);
        val.appendDate(CollectionType::DEPRECATED_lastmod(), jsTime());
        val.appendBool(CollectionType::dropped(), _dropped);
        if ( _cm ) {
            // This also appends the lastmodEpoch.
            _cm->getInfo( val );
        }
        else {
            // lastmodEpoch is a required field so we also need to do it here.
            val.append(CollectionType::DEPRECATED_lastmodEpoch(), ChunkVersion::DROPPED.epoch());
        }

        conn->update(CollectionType::ConfigNS, key, val.obj(), true);
        string err = conn->getLastError();
        uassert( 13473 , (string)"failed to save collection (" + ns + "): " + err , err.size() == 0 );

        _dirty = false;
    }

    bool DBConfig::isSharded( const string& ns ) {
        if ( ! _shardingEnabled )
            return false;
        scoped_lock lk( _lock );
        return _isSharded( ns );
    }

    bool DBConfig::_isSharded( const string& ns ) {
        if ( ! _shardingEnabled )
            return false;
        Collections::iterator i = _collections.find( ns );
        if ( i == _collections.end() )
            return false;
        return i->second.isSharded();
    }

    ShardPtr DBConfig::getShardIfExists( const string& ns ){
        try{
            // TODO: this function assumes the _primary will not change under-the-covers, but so does
            // getShard() in general
            return ShardPtr( new Shard( getShard( ns ) ) );
        }
        catch( AssertionException& e ){
            warning() << "primary not found for " << ns << causedBy( e ) << endl;
            return ShardPtr();
        }
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

        scoped_lock lk( _lock );
        _shardingEnabled = true;
        if( save ) _save();
    }

    /**
     *
     */
    ChunkManagerPtr DBConfig::shardCollection( const string& ns , ShardKeyPattern fieldsAndOrder , bool unique , vector<BSONObj>* initPoints, vector<Shard>* initShards ) {
        uassert( 8042 , "db doesn't have sharding enabled" , _shardingEnabled );
        uassert( 13648 , str::stream() << "can't shard collection because not all config servers are up" , configServer.allUp() );

        ChunkManagerPtr manager;
        
        {
            scoped_lock lk( _lock );

            CollectionInfo& ci = _collections[ns];
            uassert( 8043 , "collection already sharded" , ! ci.isSharded() );

            log() << "enable sharding on: " << ns << " with shard key: " << fieldsAndOrder << endl;

            ChunkManager* cm = new ChunkManager( ns, fieldsAndOrder, unique );
            cm->createFirstChunks( configServer.getPrimary().getConnString(),
                                   getPrimary(), initPoints, initShards );
            ci.shard( cm );

            _save();

            // Save the initial chunk manager for later, no need to reload if we're in this lock
            manager = ci.getCM();
            verify( manager.get() );
        }

        // Tell the primary mongod to refresh it's data
        // TODO:  Think the real fix here is for mongos to just assume all collections sharded, when we get there
        for( int i = 0; i < 4; i++ ){
            if( i == 3 ){
                warning() << "too many tries updating initial version of " << ns << " on shard primary " << getPrimary() <<
                             ", other mongoses may not see the collection as sharded immediately" << endl;
                break;
            }
            try {
                ShardConnection conn( getPrimary(), ns );
                conn.setVersion();
                conn.done();
                break;
            }
            catch( DBException& e ){
                warning() << "could not update initial version of " << ns << " on shard primary " << getPrimary() <<
                             causedBy( e ) << endl;
            }
            sleepsecs( i );
        }

        return manager;
    }

    bool DBConfig::removeSharding( const string& ns ) {
        if ( ! _shardingEnabled ) {

            warning() << "could not remove sharding for collection " << ns
                      << ", sharding not enabled for db" << endl;
            return false;
        }

        scoped_lock lk( _lock );

        Collections::iterator i = _collections.find( ns );

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
    void DBConfig::getChunkManagerOrPrimary( const string& ns, ChunkManagerPtr& manager, ShardPtr& primary ){

        // The logic here is basically that at any time, our collection can become sharded or unsharded
        // via a command.  If we're not sharded, we want to send data to the primary, if sharded, we want
        // to send data to the correct chunks, and we can't check both w/o the lock.

        manager.reset();
        primary.reset();

        {
            scoped_lock lk( _lock );

            Collections::iterator i = _collections.find( ns );

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
        try{
            return getChunkManager( ns, shouldReload, forceReload );
        }
        catch( AssertionException& e ){
            warning() << "chunk manager not found for " << ns << causedBy( e ) << endl;
            return ChunkManagerPtr();
        }
    }

    ChunkManagerPtr DBConfig::getChunkManager( const string& ns , bool shouldReload, bool forceReload ) {
        BSONObj key;
        ChunkVersion oldVersion;
        ChunkManagerPtr oldManager;

        {
            scoped_lock lk( _lock );
            
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

        BSONObj newest;
        if ( oldVersion.isSet() && ! forceReload ) {
            ScopedDbConnection conn(configServer.modelServer(), 30.0);
            newest = conn->findOne(ChunkType::ConfigNS,
                                   Query(BSON(ChunkType::ns(ns))).sort(
                                           ChunkType::DEPRECATED_lastmod(), -1));
            conn.done();
            
            if ( ! newest.isEmpty() ) {
                ChunkVersion v = ChunkVersion::fromBSON(newest, ChunkType::DEPRECATED_lastmod());
                if ( v.isEquivalentTo( oldVersion ) ) {
                    scoped_lock lk( _lock );
                    CollectionInfo& ci = _collections[ns];
                    uassert( 15885 , str::stream() << "not sharded after reloading from chunks : " << ns , ci.isSharded() );
                    return ci.getCM();
                }
            }

        }
        else if( ! oldVersion.isSet() ){
            warning() << "version 0 found when " << ( forceReload ? "reloading" : "checking" ) << " chunk manager"
                      << ", collection '" << ns << "' initially detected as sharded" << endl;
        }

        // we are not locked now, and want to load a new ChunkManager
        
        auto_ptr<ChunkManager> temp;

        {
            scoped_lock lll ( _hitConfigServerLock );
            
            if ( ! newest.isEmpty() && ! forceReload ) {
                // if we have a target we're going for
                // see if we've hit already
                
                scoped_lock lk( _lock );
                CollectionInfo& ci = _collections[ns];
                if ( ci.isSharded() && ci.getCM() ) {

                    ChunkVersion currentVersion =
                        ChunkVersion::fromBSON(newest, ChunkType::DEPRECATED_lastmod());

                    // Only reload if the version we found is newer than our own in the same
                    // epoch
                    if( currentVersion <= ci.getCM()->getVersion() &&
                        ci.getCM()->getVersion().hasCompatibleEpoch( currentVersion ) )
                    {
                        return ci.getCM();
                    }
                }
                
            }
            
            temp.reset( new ChunkManager( oldManager ) );
            temp->loadExistingRanges( configServer.getPrimary().getConnString() );

            if ( temp->numChunks() == 0 ) {
                // maybe we're not sharded any more
                reload(); // this is a full reload
                return getChunkManager( ns , false );
            }
        }

        scoped_lock lk( _lock );
        
        CollectionInfo& ci = _collections[ns];
        uassert( 14822 ,  (string)"state changed in the middle: " + ns , ci.isSharded() );

        // Reset if our versions aren't the same
        bool shouldReset = ! temp->getVersion().isEquivalentTo( ci.getCM()->getVersion() );
        
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
        scoped_lock lk( _lock );
        _primary.reset( s );
        _save();
    }

    void DBConfig::serialize(BSONObjBuilder& to) {
        to.append("_id", _name);
        to.appendBool(DatabaseType::DEPRECATED_partitioned(), _shardingEnabled );
        to.append(DatabaseType::primary(), _primary.getName() );
    }

    void DBConfig::unserialize(const BSONObj& from) {
        LOG(1) << "DBConfig unserialize: " << _name << " " << from << endl;
        verify( _name == from[DatabaseType::name()].String() );

        _shardingEnabled = from.getBoolField(DatabaseType::DEPRECATED_partitioned().c_str());
        _primary.reset( from.getStringField(DatabaseType::primary().c_str()));

        // In the 1.5.x series, we used to have collection metadata nested in the database entry. The 1.6.x series
        // had migration code that ported that info to where it belongs now: the 'collections' collection. We now
        // just assert that we're not migrating from a 1.5.x directly into a 1.7.x without first converting.
        BSONObj sharded = from.getObjectField(DatabaseType::DEPRECATED_sharded().c_str());
        if ( ! sharded.isEmpty() )
            uasserted( 13509 , "can't migrate from 1.5.x release to the current one; need to upgrade to 1.6.x first");
    }

    bool DBConfig::load() {
        scoped_lock lk( _lock );
        return _load();
    }

    bool DBConfig::_load() {
        ScopedDbConnection conn(configServer.modelServer(), 30.0);

        BSONObj dbObj = conn->findOne( DatabaseType::ConfigNS,
                                       BSON( DatabaseType::name( _name ) ) );

        if ( dbObj.isEmpty() ) {
            conn.done();
            return false;
        }

        unserialize( dbObj );

        BSONObjBuilder b;
        b.appendRegex(CollectionType::ns(),
                      (string)"^" + pcrecpp::RE::QuoteMeta( _name ) + "\\." );

        int numCollsErased = 0;
        int numCollsSharded = 0;

        auto_ptr<DBClientCursor> cursor = conn->query(CollectionType::ConfigNS, b.obj());
        verify( cursor.get() );
        while ( cursor->more() ) {

            BSONObj collObj = cursor->next();
            string collName = collObj[CollectionType::ns()].String();

            if( collObj[CollectionType::dropped()].trueValue() ){
                _collections.erase( collName );
                numCollsErased++;
            }
            else if( !collObj[CollectionType::primary()].eoo() ){
                // For future compatibility, explicitly ignore any collection with the
                // "primary" field set.

                // Erased in case it was previously sharded, dropped, then init'd as unsharded
                _collections.erase( collName );
                numCollsErased++;
            }
            else{
                _collections[ collName ] = CollectionInfo( collObj );
                if( _collections[ collName ].isSharded() ) numCollsSharded++;
            }
        }

        LOG(2) << "found " << numCollsErased << " dropped collections and "
               << numCollsSharded << " sharded collections for database " << _name << endl;

        conn.done();

        return true;
    }

    void DBConfig::_save( bool db, bool coll ) {
        ScopedDbConnection conn(configServer.modelServer(), 30.0);

        if( db ){

            BSONObj n;
            {
                BSONObjBuilder b;
                serialize(b);
                n = b.obj();
            }

            conn->update( DatabaseType::ConfigNS , BSON( DatabaseType::name( _name ) ) , n , true );
            string err = conn->getLastError();
            uassert( 13396 , (string)"DBConfig save failed: " + err , err.size() == 0 );

        }

        if( coll ){

            for ( Collections::iterator i=_collections.begin(); i!=_collections.end(); ++i ) {
                if ( ! i->second.isDirty() )
                    continue;
                i->second.save( i->first , conn.get() );
            }

        }

        conn.done();
    }

    bool DBConfig::reload() {
        bool successful = false;

        {
            scoped_lock lk( _lock );
            successful = _reload();
        }

        //
        // If we aren't successful loading the database entry, we don't want to keep the stale
        // object around which has invalid data.  We should remove it instead.
        //

        if( ! successful ) grid.removeDBIfExists( *this );

        return successful;
    }

    bool DBConfig::_reload() {
        // TODO: i don't think is 100% correct
        return _load();
    }

    bool DBConfig::dropDatabase( string& errmsg ) {
        /**
         * 1) make sure everything is up
         * 2) update config server
         * 3) drop and reset sharded collections
         * 4) drop and reset primary
         * 5) drop everywhere to clean up loose ends
         */

        log() << "DBConfig::dropDatabase: " << _name << endl;
        configServer.logChange( "dropDatabase.start" , _name , BSONObj() );

        // 1
        if ( ! configServer.allUp( errmsg ) ) {
            LOG(1) << "\t DBConfig::dropDatabase not all up" << endl;
            return 0;
        }

        // 2
        grid.removeDB( _name );
        {
            ScopedDbConnection conn(configServer.modelServer(), 30.0);
            conn->remove( DatabaseType::ConfigNS , BSON( DatabaseType::name( _name ) ) );
            errmsg = conn->getLastError();
            if ( ! errmsg.empty() ) {
                log() << "could not drop '" << _name << "': " << errmsg << endl;
                conn.done();
                return false;
            }

            conn.done();
        }

        if ( ! configServer.allUp( errmsg ) ) {
            log() << "error removing from config server even after checking!" << endl;
            return 0;
        }
        LOG(1) << "\t removed entry from config server for: " << _name << endl;

        set<Shard> allServers;

        // 3
        while ( true ) {
            int num = 0;
            if ( ! _dropShardedCollections( num , allServers , errmsg ) )
                return 0;
            log() << "   DBConfig::dropDatabase: " << _name << " dropped sharded collections: " << num << endl;
            if ( num == 0 )
                break;
        }

        // 4
        {
            ScopedDbConnection conn(_primary.getConnString(), 30.0);
            BSONObj res;
                if ( ! conn->dropDatabase( _name , &res ) ) {
                errmsg = res.toString();
                return 0;
            }
            conn.done();
        }

        // 5
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

        configServer.logChange( "dropDatabase" , _name , BSONObj() );
        return true;
    }

    bool DBConfig::_dropShardedCollections( int& num, set<Shard>& allServers , string& errmsg ) {
        num = 0;
        set<string> seen;
        while ( true ) {
            Collections::iterator i = _collections.begin();
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
            i->second.getCM()->drop( i->second.getCM() );

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

    void DBConfig::getAllShards(set<Shard>& shards) const {
        scoped_lock lk( _lock );
        shards.insert(getPrimary());
        for (Collections::const_iterator it(_collections.begin()), end(_collections.end()); it != end; ++it) {
            if (it->second.isSharded()) {
                it->second.getCM()->getAllShards(shards);
            } // TODO: handle collections on non-primary shard
        }
    }

    void DBConfig::getAllShardedCollections( set<string>& namespaces ) const {

        scoped_lock lk( _lock );

        for( Collections::const_iterator i = _collections.begin(); i != _collections.end(); i++ ) {
            log() << "Coll : " << i->first << " sharded? " << i->second.isSharded() << endl;
            if( i->second.isSharded() ) namespaces.insert( i->first );
        }

    }

    /* --- ConfigServer ---- */

    ConfigServer::ConfigServer() : DBConfig( "config" ) {
        _shardingEnabled = false;
    }

    ConfigServer::~ConfigServer() {
    }

    bool ConfigServer::init( const std::string& s ) {
        vector<string> configdbs;
        splitStringDelim( s, &configdbs, ',' );
        return init( configdbs );
    }

    bool ConfigServer::init( vector<string> configHosts ) {

        uassert( 10187 ,  "need configdbs" , configHosts.size() );

        string hn = getHostName();
        if ( hn.empty() ) {
            sleepsecs(5);
            dbexit( EXIT_BADOPTIONS );
        }

        set<string> hosts;
        for ( size_t i=0; i<configHosts.size(); i++ ) {
            string host = configHosts[i];
            hosts.insert( getHost( host , false ) );
            configHosts[i] = getHost( host , true );
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

        string fullString;
        joinStringDelim( configHosts, &fullString, ',' );
        _primary.setAddress( ConnectionString( fullString , ConnectionString::SYNC ) );
        LOG(1) << " config string : " << fullString << endl;

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

    bool ConfigServer::checkConfigServersConsistent( string& errmsg , int tries ) const {
        if ( tries <= 0 )
            return false;

        unsigned firstGood = 0;
        int up = 0;
        vector<BSONObj> res;
        // The last error we saw on a config server
        string error;
        for ( unsigned i=0; i<_config.size(); i++ ) {
            BSONObj result;

            scoped_ptr<ScopedDbConnection> conn;

            try {
                conn.reset( new ScopedDbConnection( _config[i], 30.0 ) );

                if ( ! conn->get()->runCommand( "config",
                                                BSON( "dbhash" << 1 <<
                                                      "collections" << BSON_ARRAY( "chunks" <<
                                                                                   "databases" ) ),
                                                result ) ) {

                    // TODO: Make this a helper
                    error = result["errmsg"].eoo() ? "" : result["errmsg"].String();
                    if (!result["assertion"].eoo()) error = result["assertion"].String();

                    warning() << "couldn't check dbhash on config server " << _config[i] 
                              << causedBy(result.toString()) << endl;

                    result = BSONObj();
                }
                else {
                    result = result.getOwned();
                    if ( up == 0 )
                        firstGood = i;
                    up++;
                }
                conn->done();
            }
            catch ( const DBException& e ) {
                if (conn) {
                    conn->kill();
                }

                // We need to catch DBExceptions b/c sometimes we throw them
                // instead of socket exceptions when findN fails

                error = e.toString();
                warning() << " couldn't check dbhash on config server " << _config[i] << causedBy(e) << endl;
            }
            res.push_back(result);
        }

        if ( _config.size() == 1 )
            return true;

        if ( up == 0 ) {
            // Use a ptr to error so if empty we won't add causedby
            errmsg = str::stream() << "no config servers successfully contacted" << causedBy(&error);
            return false;
        }

        if ( up == 1 ) {
            warning() << "only 1 config server reachable, continuing" << endl;
            return true;
        }

        BSONObj base = res[firstGood];
        for ( unsigned i=firstGood+1; i<res.size(); i++ ) {
            if ( res[i].isEmpty() )
                continue;

            string c1 = base.getFieldDotted( "collections.chunks" );
            string c2 = res[i].getFieldDotted( "collections.chunks" );

            string d1 = base.getFieldDotted( "collections.databases" );
            string d2 = res[i].getFieldDotted( "collections.databases" );

            if ( c1 == c2 && d1 == d2 )
                continue;

            stringstream ss;
            ss << "config servers " << _config[firstGood] << " and " << _config[i] << " differ";
            warning() << ss.str() << endl;
            if ( tries <= 1 ) {
                ss << "\n" << c1 << "\t" << c2 << "\n" << d1 << "\t" << d2;
                errmsg = ss.str();
                return false;
            }

            return checkConfigServersConsistent( errmsg , tries - 1 );
        }

        return true;
    }

    bool ConfigServer::ok( bool checkConsistency ) {
        if ( ! _primary.ok() )
            return false;

        if ( checkConsistency ) {
            string errmsg;
            if ( ! checkConfigServersConsistent( errmsg ) ) {
                error() << "could not verify that config servers are in sync" << causedBy(errmsg) << warnings;
                return false;
            }
        }

        return true;
    }

    bool ConfigServer::allUp() {
        string errmsg;
        return allUp( errmsg );
    }

    bool ConfigServer::allUp( string& errmsg ) {
        try {
            ScopedDbConnection conn(_primary.getConnString(), 30.0);
            conn->getLastError();
            conn.done();
            return true;
        }
        catch ( DBException& ) {
            log() << "ConfigServer::allUp : " << _primary.toString() << " seems down!" << endl;
            errmsg = _primary.toString() + " seems down";
            return false;
        }

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
            if ( conn.count(ShardType::ConfigNS) || conn.count( DatabaseType::ConfigNS ) ) {
                version = 1;
            }
        }

        return version;
    }

    void ConfigServer::reloadSettings() {
        set<string> got;

        ScopedDbConnection conn(_primary.getConnString(), 30.0);
        
        try {
            
            auto_ptr<DBClientCursor> c = conn->query( SettingsType::ConfigNS , BSONObj() );
            verify( c.get() );
            while ( c->more() ) {
                
                BSONObj o = c->next();
                string name = o[SettingsType::key()].valuestrsafe();
                got.insert( name );
                if ( name == "chunksize" ) {
                    int csize = o[SettingsType::chunksize()].numberInt();

                    // validate chunksize before proceeding
                    if ( csize == 0 ) {
                        // setting was not modified; mark as such
                        got.erase(name);
                        log() << "warning: invalid chunksize (" << csize << ") ignored" << endl;
                    } else {
                        LOG(1) << "MaxChunkSize: " << csize << endl;
                        if ( !Chunk::setMaxChunkSizeSizeMB( csize ) ) {
                            warning() << "invalid chunksize: " << csize << endl;
                        }
                    }
                }
                else if ( name == "balancer" ) {
                    // ones we ignore here
                }
                else {
                    log() << "warning: unknown setting [" << name << "]" << endl;
                }
            }

            if ( ! got.count( "chunksize" ) ) {
                conn->insert(SettingsType::ConfigNS,
                                     BSON(SettingsType::key("chunksize") <<
                                          SettingsType::chunksize(Chunk::MaxChunkSize /
                                                                    (1024 * 1024))));
            }

            // indexes
            conn->ensureIndex(ChunkType::ConfigNS,
                                     BSON(ChunkType::ns() << 1 << ChunkType::min() << 1 ), true);

            conn->ensureIndex(ChunkType::ConfigNS,
                                     BSON(ChunkType::ns() << 1 <<
                                          ChunkType::shard() << 1 <<
                                          ChunkType::min() << 1 ), true);

            conn->ensureIndex(ChunkType::ConfigNS,
                                     BSON(ChunkType::ns() << 1 <<
                                          ChunkType::DEPRECATED_lastmod() << 1 ), true );

            conn->ensureIndex(ShardType::ConfigNS, BSON(ShardType::host() << 1), true);

            conn.done();
        }
        catch ( DBException& e ) {
            warning() << "couldn't load settings or create indexes on config db: " << e.what() << endl;
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
            ss << name << ":" << CmdLine::ConfigServerPort;
            return ss.str();
        }

        return name;
    }

    /* must never throw */
    void ConfigServer::logChange( const string& what , const string& ns , const BSONObj& detail ) {
        string changeID;

        try {
            // get this entry's ID so we can use on the exception code path too
            stringstream id;
            id << getHostNameCached() << "-" << terseCurrentTime() << "-" << OID::gen();
            changeID = id.str();

            // send a copy of the message to the log in case it doesn't manage to reach config.changelog
            Client* c = currentClient.get();
            BSONObj msg = BSON( ChangelogType::changeID(changeID) <<
                                ChangelogType::server(getHostNameCached()) <<
                                ChangelogType::clientAddr((c ? c->clientAddress(true) : "N/A")) <<
                                ChangelogType::time(jsTime()) <<
                                ChangelogType::what(what) <<
                                ChangelogType::ns(ns) <<
                                ChangelogType::details(detail) );
            log() << "about to log metadata event: " << msg << endl;

            verify( _primary.ok() );

            ScopedDbConnection conn(_primary.getConnString(), 30.0);

            static bool createdCapped = false;
            if ( ! createdCapped ) {
                try {
                    conn->createCollection( ChangelogType::ConfigNS , 1024 * 1024 * 10 , true );
                }
                catch ( UserException& e ) {
                    LOG(1) << "couldn't create changelog (like race condition): " << e << endl;
                    // don't care
                }
                createdCapped = true;
            }

            conn->insert( ChangelogType::ConfigNS , msg );

            conn.done();

        }

        catch ( std::exception& e ) {
            // if we got here, it means the config change is only in the log; it didn't make it to config.changelog
            log() << "not logging config change: " << changeID << " " << e.what() << endl;
        }
    }

    void ConfigServer::replicaSetChange( const ReplicaSetMonitor * monitor ) {
        try {
            Shard s = Shard::lookupRSName(monitor->getName());
            if (s == Shard::EMPTY) {
                LOG(1) << "replicaSetChange: shard not found for set: " << monitor->getServerAddress() << endl;
                return;
            }
            ScopedDbConnection conn(configServer.getConnectionString().toString(), 30.0);
            conn->update(ShardType::ConfigNS,
                         BSON(ShardType::name(s.getName())),
                         BSON("$set" << BSON(ShardType::host(monitor->getServerAddress()))));
            conn.done();
        }
        catch (DBException& e) {
            error() << "RSChangeWatcher: could not update config db for set: " << monitor->getName()
                    << " to: " << monitor->getServerAddress() << causedBy(e) << endl;
        }
    }

    DBConfigPtr configServerPtr (new ConfigServer());
    ConfigServer& configServer = dynamic_cast<ConfigServer&>(*configServerPtr);

}
