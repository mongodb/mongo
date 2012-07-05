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

// strategy_sharded.cpp

#include "pch.h"

#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/index.h"
#include "mongo/s/client_info.h"
#include "mongo/s/chunk.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/stats.h"

// error codes 8010-8040

namespace mongo {

    class ShardStrategy : public Strategy {

        virtual void queryOp( Request& r ) {

            // TODO: These probably should just be handled here.
            if ( r.isCommand() ) {
                SINGLE->queryOp( r );
                return;
            }

            QueryMessage q( r.d() );

            r.checkAuth( Auth::READ );

            LOG(3) << "shard query: " << q.ns << "  " << q.query << endl;

            if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                throw UserException( 8010 , "something is wrong, shouldn't see a command here" );

            QuerySpec qSpec( (string)q.ns, q.query, q.fields, q.ntoskip, q.ntoreturn, q.queryOptions );

            ParallelSortClusteredCursor * cursor = new ParallelSortClusteredCursor( qSpec, CommandInfo() );
            verify( cursor );

            // TODO:  Move out to Request itself, not strategy based
            try {
                long long start_millis = 0;
                if ( qSpec.isExplain() ) start_millis = curTimeMillis64();
                cursor->init();

                LOG(5) << "   cursor type: " << cursor->type() << endl;
                shardedCursorTypes.hit( cursor->type() );

                if ( qSpec.isExplain() ) {
                    // fetch elapsed time for the query
                    long long elapsed_millis = curTimeMillis64() - start_millis;
                    BSONObjBuilder explain_builder;
                    cursor->explain( explain_builder );
                    explain_builder.appendNumber( "millis", elapsed_millis );
                    BSONObj b = explain_builder.obj();

                    replyToQuery( 0 , r.p() , r.m() , b );
                    delete( cursor );
                    return;
                }
            }
            catch(...) {
                delete cursor;
                throw;
            }

            if( cursor->isSharded() ){
                ShardedClientCursorPtr cc (new ShardedClientCursor( q , cursor ));

                BufBuilder buffer( ShardedClientCursor::INIT_REPLY_BUFFER_SIZE );
                int docCount = 0;
                const int startFrom = cc->getTotalSent();
                bool hasMore = cc->sendNextBatch( r, q.ntoreturn, buffer, docCount );

                if ( hasMore ) {
                    LOG(5) << "storing cursor : " << cc->getId() << endl;
                    cursorCache.store( cc );
                }

                replyToQuery( 0, r.p(), r.m(), buffer.buf(), buffer.len(), docCount,
                        startFrom, hasMore ? cc->getId() : 0 );
            }
            else{
                // TODO:  Better merge this logic.  We potentially can now use the same cursor logic for everything.
                ShardPtr primary = cursor->getPrimary();
                DBClientCursorPtr shardCursor = cursor->getShardCursor( *primary );
                r.reply( *(shardCursor->getMessage()) , shardCursor->originalHost() );
            }
        }

        virtual void commandOp( const string& db, const BSONObj& command, int options,
                                const string& versionedNS, const BSONObj& filter,
                                map<Shard,BSONObj>& results )
        {
            const BSONObj& commandWithAuth = ClientBasic::getCurrent()->getAuthenticationInfo()->
                    getAuthTable().copyCommandObjAddingAuth( command );

            QuerySpec qSpec( db + ".$cmd",
                             noauth ? command : commandWithAuth,
                             BSONObj(),
                             0,
                             1,
                             options );

            ParallelSortClusteredCursor cursor( qSpec, CommandInfo( versionedNS, filter ) );

            // Initialize the cursor
            cursor.init();

            set<Shard> shards;
            cursor.getQueryShards( shards );

            for( set<Shard>::iterator i = shards.begin(), end = shards.end(); i != end; ++i ){
                results[ *i ] = cursor.getShardCursor( *i )->peekFirst().getOwned();
            }

        }

        virtual void getMore( Request& r ) {

            // TODO:  Handle stale config exceptions here from coll being dropped or sharded during op
            // for now has same semantics as legacy request
            ChunkManagerPtr info = r.getChunkManager();

            //
            // TODO: Cleanup and consolidate into single codepath
            //

            if( ! info ){

                const char *ns = r.getns();

                LOG(3) << "single getmore: " << ns << endl;

                long long id = r.d().getInt64( 4 );

                string host = cursorCache.getRef( id );

                if( host.size() == 0 ){

                    //
                    // Match legacy behavior here by throwing an exception when we can't find
                    // the cursor, but make the exception more informative
                    //

                    uasserted( 16336,
                               str::stream() << "could not find cursor in cache for id " << id
                                             << " over collection " << ns );
                }

                // we used ScopedDbConnection because we don't get about config versions
                // not deleting data is handled elsewhere
                // and we don't want to call setShardVersion
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getScopedDbConnection( host ) );

                Message response;
                bool ok = conn->get()->callRead( r.m() , response);
                uassert( 10204 , "dbgrid: getmore: error calling db", ok);
                r.reply( response , "" /*conn->getServerAddress() */ );

                conn->done();
                return;
            }
            else {
                int ntoreturn = r.d().pullInt();
                long long id = r.d().pullInt64();

                LOG(6) << "want cursor : " << id << endl;

                ShardedClientCursorPtr cursor = cursorCache.get( id );
                if ( ! cursor ) {
                    LOG(6) << "\t invalid cursor :(" << endl;
                    replyToQuery( ResultFlag_CursorNotFound , r.p() , r.m() , 0 , 0 , 0 );
                    return;
                }

                // TODO: Try to match logic of mongod, where on subsequent getMore() we pull lots more data?
                BufBuilder buffer( ShardedClientCursor::INIT_REPLY_BUFFER_SIZE );
                int docCount = 0;
                const int startFrom = cursor->getTotalSent();
                bool hasMore = cursor->sendNextBatch( r, ntoreturn, buffer, docCount );

                if ( hasMore ) {
                    // still more data
                    cursor->accessed();
                }
                else {
                    // we've exhausted the cursor
                    cursorCache.remove( id );
                }

                replyToQuery( 0, r.p(), r.m(), buffer.buf(), buffer.len(), docCount,
                        startFrom, hasMore ? cursor->getId() : 0 );
            }
        }

        static const int maxWaitMillis = 500;
        boost::thread_specific_ptr<Backoff> perThreadBackoff;

        /**
         * Invoked before mongos needs to throw an error relating to an operation which cannot
         * be performed on a sharded collection.
         *
         * This prevents mongos from refreshing config data too quickly in response to bad requests,
         * since doing so is expensive.
         *
         * Each thread gets its own backoff wait sequence, to avoid interfering with other valid
         * operations.
         */
        void _sleepForVerifiedLocalError(){

            if( ! perThreadBackoff.get() )
                perThreadBackoff.reset( new Backoff( maxWaitMillis, maxWaitMillis * 2 ) );

            perThreadBackoff.get()->nextSleepMillis();
        }

        void _handleRetries( const string& op,
                             int retries,
                             const string& ns,
                             const BSONObj& query,
                             StaleConfigException& e,
                             Request& r ) // TODO: remove
        {

            static const int MAX_RETRIES = 5;
            if( retries >= MAX_RETRIES ) throw e;

            //
            // On a stale config exception, we have to assume that the entire collection could have
            // become unsharded, or sharded with a different shard key - we need to re-run all the
            // targeting we've done earlier
            //

            log( retries == 0 ) << op << " will be retried b/c sharding config info is stale, "
                                << " retries: " << retries
                                << " ns: " << ns
                                << " data: " << query << endl;

            if( retries > 2 ){
                versionManager.forceRemoteCheckShardVersionCB( ns );
            }

            r.reset();
        }

        void _groupInserts( const string& ns,
                            vector<BSONObj>& inserts,
                            map<ChunkPtr,vector<BSONObj> >& insertsForChunks,
                            ChunkManagerPtr& manager,
                            ShardPtr& primary,
                            bool reloadedConfigData = false )
        {

            grid.getDBConfig( ns )->getChunkManagerOrPrimary( ns, manager, primary );

            // Redo all inserts for chunks which have changed
            map<ChunkPtr,vector<BSONObj> >::iterator i = insertsForChunks.begin();
            while( ! insertsForChunks.empty() && i != insertsForChunks.end() ){

                // If we don't have a manger, our chunk is empty, or our manager is incompatible with the chunk
                // we assigned inserts to, re-map the inserts to new chunks
                if( ! manager || ! ( i->first.get() ) || ( manager && ! manager->compatibleWith( i->first ) ) ){
                    inserts.insert( inserts.end(), i->second.begin(), i->second.end() );
                    insertsForChunks.erase( i++ );
                }
                else ++i;

            }

            // Used for storing non-sharded insert data
            ChunkPtr empty;

            // Figure out inserts we haven't chunked yet
            for( vector<BSONObj>::iterator i = inserts.begin(); i != inserts.end(); ++i ){

                BSONObj o = *i;

                if ( manager && ! manager->hasShardKey( o ) ) {

                    bool bad = true;

                    // Add autogenerated _id to item and see if we now have a shard key
                    if ( manager->getShardKey().partOfShardKey( "_id" ) ) {

                        BSONObjBuilder b;
                        b.appendOID( "_id" , 0 , true );
                        b.appendElements( o );
                        o = b.obj();
                        bad = ! manager->hasShardKey( o );

                    }

                    if( bad && ! reloadedConfigData ){

                        //
                        // The shard key may not match because it has changed on us (new collection), and we are now
                        // stale.
                        //
                        // We reload once here, to be sure that we're at least more-up-to-date than the time at
                        // which the inserts were sent.  If there's still a mismatch after that, we can and should
                        // fail so that we notify the client the cluster has changed in some way in parallel with
                        // the inserts that makes the inserts invalid.
                        //

                        //
                        // Note that each *batch* of inserts is processed this way, which makes this re-check slightly
                        // more aggressive than it needs to be if we need to rebatch for stale config, but this should
                        // be rare.
                        // Also, most inserts will be single inserts, and so a number of bad single inserts will cause
                        // the config server to be contacted repeatedly.
                        //

                        warning() << "shard key mismatch for insert " << o
                                  << ", expected values for " << manager->getShardKey()
                                  << ", reloading config data to ensure not stale" << endl;

                        // Remove all the previously grouped inserts...
                        inserts.erase( inserts.begin(), i );

                        // If this is our retry, force talking to the config server
                        grid.getDBConfig( ns )->getChunkManagerIfExists( ns, true );
                        _groupInserts( ns, inserts, insertsForChunks, manager, primary, true );
                        return;
                    }

                    if( bad ){

                        // Sleep to avoid DOS'ing config server when we have invalid inserts
                        _sleepForVerifiedLocalError();

                        // TODO: Matching old behavior, but do we need this log line?
                        log() << "tried to insert object with no valid shard key for "
                              << manager->getShardKey() << " : " << o << endl;

                        uasserted( 8011,
                              str::stream() << "tried to insert object with no valid shard key for "
                                            << manager->getShardKey().toString() << " : " << o.toString() );
                    }
                }

                // Many operations benefit from having the shard key early in the object
                if( manager ){
                    o = manager->getShardKey().moveToFront(o);
                    insertsForChunks[manager->findChunk(o)].push_back(o);
                }
                else{
                    insertsForChunks[ empty ].push_back(o);
                }
            }

            inserts.clear();
            return;
        }

        /**
         * This insert function now handes all inserts, unsharded or sharded, through mongos.
         *
         * Semantics for insert are ContinueOnError - to match mongod semantics :
         * 1) Error is thrown immediately for corrupt objects
         * 2) Error is thrown only for UserExceptions during the insert process, if last obj had error that's thrown
         */
        void _insert( Request& r , DbMessage& d ){

            const string& ns = r.getns();

            vector<BSONObj> insertsRemaining;
            while ( d.moreJSObjs() ){
                insertsRemaining.push_back( d.nextJsObj() );
            }

            int flags = 0;

            if( d.reservedField() & Reserved_InsertOption_ContinueOnError )
                flags |= InsertOption_ContinueOnError;

            if( d.reservedField() & Reserved_FromWriteback )
                flags |= WriteOption_FromWriteback;

            _insert( ns, insertsRemaining, flags, r, d );
        }

        void _insert( const string& ns,
                      vector<BSONObj>& inserts,
                      int flags,
                      Request& r , DbMessage& d ) // TODO: remove
        {
            map<ChunkPtr, vector<BSONObj> > insertsForChunks; // Map for bulk inserts to diff chunks
            _insert( ns, inserts, insertsForChunks, flags, r, d );
        }

        void _insert( const string& ns,
                      vector<BSONObj>& insertsRemaining,
                      map<ChunkPtr, vector<BSONObj> >& insertsForChunks,
                      int flags,
                      Request& r, DbMessage& d, // TODO: remove
                      int retries = 0 )
        {
            // TODO: Replace this with a better check to see if we're making progress
            uassert( 16055, str::stream() << "too many retries during bulk insert, " << insertsRemaining.size() << " inserts remaining", retries < 30 );
            uassert( 16056, str::stream() << "shutting down server during bulk insert, " << insertsRemaining.size() << " inserts remaining", ! inShutdown() );

            ChunkManagerPtr manager;
            ShardPtr primary;

            // This function handles grouping the inserts per-shard whether the collection is sharded or not.
            _groupInserts( ns, insertsRemaining, insertsForChunks, manager, primary );

            // ContinueOnError is always on when using sharding.
            flags |= manager ? InsertOption_ContinueOnError : 0;

            while( ! insertsForChunks.empty() ){

                ChunkPtr c = insertsForChunks.begin()->first;
                vector<BSONObj>& objs = insertsForChunks.begin()->second;

                //
                // Careful - if primary exists, c will be empty
                //

                const Shard& shard = c ? c->getShard() : primary.get();

                ShardConnection dbcon( shard, ns, manager );

                try {

                    LOG(4) << "inserting " << objs.size() << " documents to shard " << shard
                           << " at version "
                           << ( manager.get() ? manager->getVersion().toString() :
                                                ShardChunkVersion( 0, OID() ).toString() ) << endl;

                    // Taken from single-shard bulk insert, should not need multiple methods in future
                    // insert( c->getShard() , r.getns() , objs , flags);

                    // It's okay if the version is set here, an exception will be thrown if the version is incompatible
                    try{
                        dbcon.setVersion();
                    }
                    catch ( StaleConfigException& e ) {
                        // External try block is still needed to match bulk insert mongod
                        // behavior
                        dbcon.done();
                        _handleRetries( "insert", retries, ns, objs[0], e, r );
                        _insert( ns, insertsRemaining, insertsForChunks, flags, r, d, retries + 1 );
                        return;
                    }

                    // Certain conn types can't handle bulk inserts, so don't use unless we need to
                    if( objs.size() == 1 ){
                        dbcon->insert( ns, objs[0], flags );
                    }
                    else{
                        dbcon->insert( ns , objs , flags);
                    }

                    // TODO: Option for safe inserts here - can then use this for all inserts
                    // Not sure what this means?

                    dbcon.done();

                    int bytesWritten = 0;
                    for (vector<BSONObj>::iterator vecIt = objs.begin(); vecIt != objs.end(); ++vecIt) {
                        r.gotInsert(); // Record the correct number of individual inserts
                        bytesWritten += (*vecIt).objsize();
                    }

                    // TODO: The only reason we're grouping by chunks here is for auto-split, more efficient
                    // to track this separately and bulk insert to shards
                    if ( c && r.getClientInfo()->autoSplitOk() )
                        c->splitIfShould( bytesWritten );

                }
                catch( UserException& e ){
                    // Unexpected exception, so don't clean up the conn
                    dbcon.kill();

                    // These inserts won't be retried, as something weird happened here
                    insertsForChunks.erase( insertsForChunks.begin() );

                    // Throw if this is the last chunk bulk-inserted to
                    if( insertsForChunks.empty() ){
                        throw;
                    }

                    //
                    // WE SWALLOW THE EXCEPTION HERE BY DESIGN
                    // to match mongod behavior
                    //
                    // TODO: Make better semantics
                    //

                    warning() << "swallowing exception during batch insert"
                              << causedBy( e ) << endl;
                }

                insertsForChunks.erase( insertsForChunks.begin() );
            }
        }

        void _prepareUpdate( const string& ns,
                             const BSONObj& query,
                             const BSONObj& toUpdate,
                             int flags,
                             // Output
                             ChunkPtr& chunk,
                             ShardPtr& shard,
                             ChunkManagerPtr& manager,
                             ShardPtr& primary,
                             // Input
                             bool reloadConfigData = false )
        {
            //
            // Updates have three basic targeting options :
            // 1) Primary shard
            // 2) Single shard in collection
            // 3) All shards in cluster
            //
            // We don't (currently) target just a few shards because on retry we'd need to ensure that we didn't send
            // the update to a shard twice.
            //
            // TODO: Think this is fixable, if we better track where we're sending requests.
            // TODO: Async connection layer with error checking would make this much simpler.
            //

            // Refresh config if specified
            if( reloadConfigData ) grid.getDBConfig( ns )->getChunkManagerIfExists( ns, true );

            bool multi = flags & UpdateOption_Multi;
            bool upsert = flags & UpdateOption_Upsert;

            grid.getDBConfig( ns )->getChunkManagerOrPrimary( ns, manager, primary );

            chunk.reset();
            shard.reset();

            // Unsharded updates just go to the one primary shard
            if( ! manager ){
                shard = primary;
                return;
            }

            BSONObj shardKey;
            const ShardKeyPattern& skPattern = manager->getShardKey();

            if( toUpdate.firstElementFieldName()[0] == '$') {

                //
                // $op style update
                //

                // Validate all top-level is of style $op
                BSONForEach(op, toUpdate){

                    uassert( 16064,
                             "can't mix $operator style update with non-$op fields",
                             op.fieldName()[0] == '$' );

                    if( op.type() != Object ) continue;

                    BSONForEach( field, op.embeddedObject() ){
                        if( skPattern.partOfShardKey( field.fieldName() ) )
                            uasserted( 13123, str::stream() << "Can't modify shard key's value. field: " << field
                                                            << " collection: " << manager->getns() );
                    }
                }

                if( skPattern.hasShardKey( query ) ) shardKey = skPattern.extractKey( query );

                if( ! multi ){

                    //
                    // non-multi needs full key or _id. The _id exception because that guarantees
                    // that only one object will be updated even if we send to all shards
                    // Also, db.foo.update({_id:'asdf'}, {$inc:{a:1}}) is a common pattern that we
                    // need to allow, even if it is less efficient than if the shard key were supplied.
                    //

                    bool hasId = query.hasField( "_id" ) && getGtLtOp( query["_id"] ) == BSONObj::Equality;

                    if( ! hasId && shardKey.isEmpty() ){

                        // Retry reloading the config data once, in case the shard key
                        // has changed on us in the meantime
                        if( ! reloadConfigData ){
                            _prepareUpdate( ns, query, toUpdate, flags, chunk, shard, manager, primary, true );
                            return;
                        }

                        // Sleep here, to rate-limit the amount of bad updates we process and avoid pounding the
                        // config server
                        _sleepForVerifiedLocalError();

                        uasserted( 8013,
                          str::stream() << "For non-multi updates, must have _id or full shard key "
                                        << "(" << skPattern.toString() << ") in query" );
                    }
                }
            }
            else {

                //
                // replace style update
                //

                uassert( 16065, "multi-updates require $ops rather than replacement object", ! multi );

                if( ! skPattern.hasShardKey( toUpdate ) ){

                    // Retry reloading the config data once
                    if( ! reloadConfigData ){
                        _prepareUpdate( ns, query, toUpdate, flags, chunk, shard, manager, primary, true );
                        return;
                    }

                    // Sleep here
                    _sleepForVerifiedLocalError();

                    uasserted( 12376,
                         str::stream() << "full shard key must be in update object for collection: "
                                       << manager->getns() );
                }

                shardKey = skPattern.extractKey( toUpdate );

                BSONForEach(field, query){

                    if( ! skPattern.partOfShardKey( field.fieldName() ) || getGtLtOp( field ) != BSONObj::Equality )
                        continue;

                    if( field != shardKey[ field.fieldName() ] ){

                        // Retry reloading the config data once
                        if( ! reloadConfigData ){
                            _prepareUpdate( ns, query, toUpdate, flags, chunk, shard, manager, primary, true );
                            return;
                        }

                        // Sleep here
                        _sleepForVerifiedLocalError();

                        uasserted( 8014, str::stream() << "cannot modify shard key for collection "
                                                       << manager->getns()
                                                       << ", found new value for "
                                                       << field.fieldName() );
                    }
                }
            }

            //
            // We've now collected an exact shard key from the update or not.
            // If we've collected an exact shard key, find the chunk it goes to.
            // If we haven't collected an exact shard key, find the shard we go to
            //   If we don't have a single shard to go to, don't send back a shard
            //

            verify( manager );
            if( ! shardKey.isEmpty() ){

                chunk = manager->findChunk( shardKey );
                shard = ShardPtr( new Shard( chunk->getShard() ) );
                return;
            }
            else {

                if( upsert ){

                    // We need a shard key for upsert

                    // Retry reloading the config data once
                    if( ! reloadConfigData ){
                        _prepareUpdate( ns, query, toUpdate, flags, chunk, shard, manager, primary, true );
                        return;
                    }

                    _sleepForVerifiedLocalError();

                    uasserted( 8012, str::stream()
                        << "can't upsert something without full valid shard key : " << query );
                }

                set<Shard> shards;
                manager->getShardsForQuery( shards, query );

                verify( shards.size() > 0 );

                // Return if we have a single shard
                if( shards.size() == 1 ){
                    shard = ShardPtr( new Shard( *shards.begin() ) );
                    return;
                }

                if( query.hasField("$atomic") ){

                    // We can't run an atomic operation on more than one shard

                    // Retry reloading the config data once
                    if( ! reloadConfigData ){
                        _prepareUpdate( ns, query, toUpdate, flags, chunk, shard, manager, primary, true );
                        return;
                    }

                    _sleepForVerifiedLocalError();

                    uasserted( 13506, str::stream()
                        << "$atomic not supported sharded : " << query );
                }

                return;
            }
        }

        void _update( Request& r , DbMessage& d ){

            // const details of the request
            const string& ns = r.getns();
            int flags = d.pullInt();
            const BSONObj query = d.nextJsObj();

            uassert( 10201 ,  "invalid update" , d.moreJSObjs() );

            const BSONObj toUpdate = d.nextJsObj();

            if( d.reservedField() & Reserved_FromWriteback ){
                flags |= WriteOption_FromWriteback;
            }

            _update( ns, query, toUpdate, flags, r, d );
        }

        void _update( const string& ns,
                      const BSONObj& query,
                      const BSONObj& toUpdate,
                      int flags,
                      Request& r, DbMessage& d, // TODO: remove
                      int retries = 0 )
        {

            ChunkPtr chunk;
            ShardPtr shard;
            ChunkManagerPtr manager;
            ShardPtr primary;

            _prepareUpdate( ns, query, toUpdate, flags, chunk, shard, manager, primary );

            if( ! shard ){

                //
                // Without a shard, target all shards
                //

                //
                // data could be on more than one shard. must send to all
                // TODO: make this safer w/ shard add/remove
                //

                int* opts = (int*)( r.d().afterNS() );
                opts[0] |= UpdateOption_Broadcast; // this means don't check shard version in mongod
                broadcastWrite( dbUpdate, r );
                return;
            }

            ShardConnection dbcon( *shard, ns, manager );

            //
            // Assuming we detect on mongod sharded->unsharded and unsharded->sharded, we should
            // be okay here.
            // MovePrimary will mess this up though, along with other things.
            //

            try {
                // An exception will be thrown if the version is incompatible
                dbcon.setVersion();
            }
            catch ( StaleConfigException& e ) {

                dbcon.done();
                _handleRetries( "update", retries, ns, query, e, r );
                _update( ns, query, toUpdate, flags, r, d, retries + 1 );
                return;
            }

            dbcon->update( ns, query, toUpdate, flags );

            dbcon.done();

            if( chunk && r.getClientInfo()->autoSplitOk() )
                chunk->splitIfShould( d.msg().header()->dataLen() );
        }


        void _prepareDelete( const string& ns,
                             const BSONObj& query,
                             int flags,
                             ShardPtr& shard,
                             ChunkManagerPtr& manager,
                             ShardPtr& primary,
                             bool reloadConfigData = false )
        {

            //
            // Deletes also have three basic targeting options :
            // 1) Primary shard
            // 2) Single shard in collection
            // 3) All shards in cluster
            //
            // We don't (currently) target just a few shards because on retry we'd need to ensure that we didn't send
            // the delete to a shard twice.
            //
            // TODO: Think this is fixable, if we better track where we're sending requests.
            // TODO: Async connection layer with error checking would make this much simpler.
            //

            // Refresh config if specified
            if( reloadConfigData ) grid.getDBConfig( ns )->getChunkManagerIfExists( ns, true );

            bool justOne = flags & RemoveOption_JustOne;

            shard.reset();
            grid.getDBConfig( ns )->getChunkManagerOrPrimary( ns, manager, primary );

            if( primary ){

                shard = primary;
                return;
            }

            set<Shard> shards;
            manager->getShardsForQuery( shards, query );

            LOG(2) << "delete : " << query << " \t " << shards.size() << " justOne: " << justOne << endl;

            if( shards.size() == 1 ){

                shard = ShardPtr( new Shard( *shards.begin() ) );
                return;
            }

            if( query.hasField( "$atomic" ) ){

                // Retry reloading the config data once
                if( ! reloadConfigData ){
                    _prepareDelete( ns, query, flags, shard, manager, primary, true );
                    return;
                }

                // Sleep so we don't DOS config server
                _sleepForVerifiedLocalError();

                uasserted( 13505, str::stream()
                    << "$atomic not supported for sharded delete : " << query );
            }
            else if( justOne && ! query.hasField( "_id" ) ){

                // Retry reloading the config data once
                if( ! reloadConfigData ){
                    _prepareDelete( ns, query, flags, shard, manager, primary, true );
                    return;
                }

                // Sleep so we don't DOS config server
                _sleepForVerifiedLocalError();

                uasserted( 8015, str::stream()
                    << "can only delete with a non-shard key pattern if can " <<
                       "delete as many as we find : " << query );
            }
        }

        void _delete( Request& r , DbMessage& d ) {

            // details of the request
            const string& ns = r.getns();
            int flags = d.pullInt();

            uassert( 10203 ,  "bad delete message" , d.moreJSObjs() );

            const BSONObj query = d.nextJsObj();

            if( d.reservedField() & Reserved_FromWriteback ){
                flags |= WriteOption_FromWriteback;
            }

            _delete( ns, query, flags, r, d );
        }

        void _delete( const string &ns,
                      const BSONObj& query,
                      int flags,
                      Request& r, DbMessage& d, // todo : remove
                      int retries = 0 )
        {
            ShardPtr shard;
            ChunkManagerPtr manager;
            ShardPtr primary;

            _prepareDelete( ns, query, flags, shard, manager, primary );

            if( ! shard ){

                int * x = (int*)(r.d().afterNS());
                x[0] |= RemoveOption_Broadcast; // this means don't check shard version in mongod
                // TODO: Why is this an update op here?
                broadcastWrite( dbUpdate, r );
                return;
            }

            ShardConnection dbcon( *shard, ns, manager );

            try {
                // An exception will be thrown if the version is incompatible
                dbcon.setVersion();
            }
            catch ( StaleConfigException& e ) {
                dbcon.done();
                _handleRetries( "delete", retries, ns, query, e, r );
                _delete( ns, query, flags, r, d, retries + 1 );
                return;
            }

            dbcon->remove( ns, query, flags );

            dbcon.done();
        }



        virtual void writeOp( int op , Request& r ) {

            const char *ns = r.getns();

            // TODO: Index write logic needs to be audited
            bool isIndexWrite =
                    strstr( ns , ".system.indexes" ) == strchr( ns , '.' ) && strchr( ns , '.' );

            // TODO: This block goes away, system.indexes needs to handle better
            if( isIndexWrite ){

                if ( r.getConfig()->isShardingEnabled() ){
                    LOG(1) << "sharded index write for " << ns << endl;
                    handleIndexWrite( op , r );
                    return;
                }

                LOG(3) << "single index write for " << ns << endl;
                SINGLE->doWrite( op , r , Shard( r.getConfig()->getPrimary() ) );
                r.gotInsert(); // Won't handle mulit-insert correctly. Not worth parsing the request.

                return;
            }
            else{

                LOG(3) << "write: " << ns << endl;

                DbMessage& d = r.d();

                if ( op == dbInsert ) {
                    _insert( r , d );
                }
                else if ( op == dbUpdate ) {
                    _update( r , d );
                }
                else if ( op == dbDelete ) {
                    _delete( r , d );
                }
                else {
                    log() << "sharding can't do write op: " << op << endl;
                    throw UserException( 8016 , "can't do this write op on sharded collection" );
                }
                return;
            }
        }

        void handleIndexWrite( int op , Request& r ) {

            DbMessage& d = r.d();

            if ( op == dbInsert ) {
                while( d.moreJSObjs() ) {
                    BSONObj o = d.nextJsObj();
                    const char * ns = o["ns"].valuestr();
                    if ( r.getConfig()->isSharded( ns ) ) {
                        BSONObj newIndexKey = o["key"].embeddedObjectUserCheck();

                        uassert( 10205 ,  (string)"can't use unique indexes with sharding  ns:" + ns +
                                 " key: " + o["key"].embeddedObjectUserCheck().toString() ,
                                 IndexDetails::isIdIndexPattern( newIndexKey ) ||
                                 ! o["unique"].trueValue() ||
                                 r.getConfig()->getChunkManager( ns )->getShardKey().isPrefixOf( newIndexKey ) );

                        ChunkManagerPtr cm = r.getConfig()->getChunkManager( ns );
                        verify( cm );

                        set<Shard> shards;
                        cm->getAllShards(shards);
                        for (set<Shard>::const_iterator it=shards.begin(), end=shards.end(); it != end; ++it)
                            SINGLE->doWrite( op , r , *it );
                    }
                    else {
                        SINGLE->doWrite( op , r , r.primaryShard() );
                    }
                    r.gotInsert();
                }
            }
            else if ( op == dbUpdate ) {
                uasserted( 8050 , "can't update system.indexes" );
            }
            else if ( op == dbDelete ) {
                uasserted( 8051 , "can't delete indexes on sharded collection yet" );
            }
            else {
                log() << "handleIndexWrite invalid write op: " << op << endl;
                uasserted( 8052 , "handleIndexWrite invalid write op" );
            }

        }

    };

    Strategy * SHARDED = new ShardStrategy();
}
