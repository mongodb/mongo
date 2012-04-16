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
#include "mongo/s/client.h"
#include "mongo/s/cursors.h"
#include "mongo/s/request.h"
#include "mongo/s/stats.h"
#include "mongo/s/chunk.h"

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
                bool hasMore = cc->sendNextBatch( r, q.ntoreturn, buffer, docCount );

                if ( hasMore ) {
                    LOG(5) << "storing cursor : " << cc->getId() << endl;
                    cursorCache.store( cc );
                }

                replyToQuery( 0, r.p(), r.m(), buffer.buf(), buffer.len(), docCount,
                        cc->getTotalSent(), hasMore ? cc->getId() : 0 );
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

            QuerySpec qSpec( db + ".$cmd", command, BSONObj(), 0, 1, options );

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

            if( ! info ){
                SINGLE->getMore( r );
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
                        cursor->getTotalSent(), hasMore ? cursor->getId() : 0 );
            }
        }

        void _groupInserts( ChunkManagerPtr manager, vector<BSONObj>& inserts, map<ChunkPtr,vector<BSONObj> >& insertsForChunks ){

            // Redo all inserts for chunks which have changed
            map<ChunkPtr,vector<BSONObj> >::iterator i = insertsForChunks.begin();
            while( ! insertsForChunks.empty() && i != insertsForChunks.end() ){
                if( ! manager->compatibleWith( i->first ) ){
                    inserts.insert( inserts.end(), i->second.begin(), i->second.end() );
                    insertsForChunks.erase( i++ );
                }
                else ++i;
            }

            // Figure out inserts we haven't chunked yet
            for( vector<BSONObj>::iterator i = inserts.begin(); i != inserts.end(); ++i ){

                BSONObj o = *i;

                if ( ! manager->hasShardKey( o ) ) {

                    bool bad = true;

                    // Add autogenerated _id to item and see if we now have a shard key
                    if ( manager->getShardKey().partOfShardKey( "_id" ) ) {
                        BSONObjBuilder b;
                        b.appendOID( "_id" , 0 , true );
                        b.appendElements( o );
                        o = b.obj();
                        bad = ! manager->hasShardKey( o );
                    }

                    if ( bad ) {
                        // TODO:
                        log() << "tried to insert object with no valid shard key for " << manager->getShardKey() << " : " << o << endl;
                        uassert( 8011, str::stream() << "tried to insert object with no valid shard key for " << manager->getShardKey().toString() << " : " << o.toString(), false );
                    }
                }

                // Many operations benefit from having the shard key early in the object
                o = manager->getShardKey().moveToFront(o);
                insertsForChunks[manager->findChunk(o)].push_back(o);
            }

            inserts.clear();
        }

        void _insert( Request& r , DbMessage& d, ChunkManagerPtr manager, vector<BSONObj>& insertsRemaining, map<ChunkPtr, vector<BSONObj> > insertsForChunks, int retries = 0 ) {

            uassert( 16055, str::stream() << "too many retries during bulk insert, " << insertsRemaining.size() << " inserts remaining", retries < 30 );
            uassert( 16056, str::stream() << "shutting down server during bulk insert, " << insertsRemaining.size() << " inserts remaining", ! inShutdown() );

            const int flags = d.reservedField() | InsertOption_ContinueOnError; // ContinueOnError is always on when using sharding.

            _groupInserts( manager, insertsRemaining, insertsForChunks );

            while( ! insertsForChunks.empty() ){

                ChunkPtr c = insertsForChunks.begin()->first;
                vector<BSONObj>& objs = insertsForChunks.begin()->second;

                const Shard& shard = c->getShard();
                const string& ns = r.getns();

                ShardConnection dbcon( shard, ns, manager );

                try {

                    LOG(4) << "  server:" << c->getShard().toString() << " bulk insert " << objs.size() << " documents" << endl;

                    // Taken from single-shard bulk insert, should not need multiple methods in future
                    // insert( c->getShard() , r.getns() , objs , flags);

                    // It's okay if the version is set here, an exception will be thrown if the version is incompatible
                    dbcon.setVersion();

                    dbcon->insert( ns , objs , flags);
                    // TODO: Option for safe inserts here - can then use this for all inserts

                    dbcon.done();

                    int bytesWritten = 0;
                    for (vector<BSONObj>::iterator vecIt = objs.begin(); vecIt != objs.end(); ++vecIt) {
                        r.gotInsert(); // Record the correct number of individual inserts
                        bytesWritten += (*vecIt).objsize();
                    }

                    // TODO: The only reason we're grouping by chunks here is for auto-split, more efficient
                    // to track this separately and bulk insert to shards
                    if ( r.getClientInfo()->autoSplitOk() )
                        c->splitIfShould( bytesWritten );

                }
                catch ( StaleConfigException& e ) {
                    // Cleanup the connection
                    dbcon.done();

                    // Assume the inserts did *not* succeed, so we don't want to erase them

                    int logLevel = retries < 2;
                    LOG( logLevel ) << "retrying bulk insert of " << objs.size() << " documents to chunk " << c << " because of StaleConfigException: " << e << endl;

                    if( retries > 2 ){
                        versionManager.forceRemoteCheckShardVersionCB( e.getns() );
                    }

                    // TODO:  Replace with actual chunk handling code, simplify request
                    r.reset();
                    manager = r.getChunkManager();

                    if( ! manager ) {
                        // TODO : We can probably handle this better?
                        uasserted( 14804, "collection no longer sharded" );
                    }
                    // End TODO

                    // We may need to regroup at least some of our inserts since our chunk manager may have changed
                    _insert( r, d, manager, insertsRemaining, insertsForChunks, retries + 1 );
                    return;
                }
                catch( UserException& ){
                    // Unexpected exception, so don't clean up the conn
                    dbcon.kill();

                    // These inserts won't be retried, as something weird happened here
                    insertsForChunks.erase( insertsForChunks.begin() );

                    // Throw if this is the last chunk bulk-inserted to
                    if( insertsForChunks.empty() ){
                        throw;
                    }
                }

                insertsForChunks.erase( insertsForChunks.begin() );
            }
        }

        /**
         * Semantics for insert are ContinueOnError - to match mongod semantics :
         * 1) Error is thrown immediately for corrupt objects
         * 2) Error is thrown only for UserExceptions during the insert process, if last obj had error that's thrown
         */
        void _insert( Request& r , DbMessage& d, ChunkManagerPtr manager ){

            vector<BSONObj> insertsRemaining;
            while ( d.moreJSObjs() ){
                insertsRemaining.push_back( d.nextJsObj() );
            }

            map<ChunkPtr, vector<BSONObj> > insertsForChunks; // Map for bulk inserts to diff chunks

            _insert( r, d, manager, insertsRemaining, insertsForChunks );
        }

        void _update( Request& r , DbMessage& d, ChunkManagerPtr manager ) {
            // const details of the request
            const int flags = d.pullInt();
            const BSONObj query = d.nextJsObj();
            uassert( 10201 ,  "invalid update" , d.moreJSObjs() );
            const BSONObj toupdate = d.nextJsObj();
            const bool upsert = flags & UpdateOption_Upsert;
            const bool multi = flags & UpdateOption_Multi;

            uassert( 13506 ,  "$atomic not supported sharded" , !query.hasField("$atomic") );

            // This are used for routing the request and are listed in order of priority
            // If one is empty, go to next. If both are empty, send to all shards
            BSONObj key; // the exact shard key
            BSONObj chunkFinder; // a query listing

            const ShardKeyPattern& sk = manager->getShardKey();

            if (toupdate.firstElementFieldName()[0] == '$') { // $op style update
                chunkFinder = query;

                BSONForEach(op, toupdate){
                    // this block is all about validation
                    uassert(16064, "can't mix $operator style update with non-$op fields", op.fieldName()[0] == '$');
                    if (op.type() != Object)
                        continue;
                    BSONForEach(field, op.embeddedObject()){
                        if (sk.partOfShardKey(field.fieldName()))
                            uasserted(13123, str::stream() << "Can't modify shard key's value. field: " << field
                                                            << " collection: " << manager->getns());
                    }
                }

                if (sk.hasShardKey(query))
                    key = sk.extractKey(query);

                if (!multi){
                    // non-multi needs full key or _id. The _id exception because that guarantees
                    // that only one object will be updated even if we send to all shards
                    // Also, db.foo.update({_id:'asdf'}, {$inc:{a:1}}) is a common pattern that we
                    // need to allow, even if it is less efficient than if the shard key were supplied.
                    const bool hasId = query.hasField("_id") && getGtLtOp(query["_id"]) == BSONObj::Equality;
                    uassert(8013, "For non-multi updates, must have _id or full shard key in query", hasId || !key.isEmpty());
                }
            }
            else { // replace style update
                uassert(16065, "multi-updates require $ops rather than replacement object", !multi);

                uassert(12376, str::stream() << "full shard key must be in update object for collection: " << manager->getns(),
                        sk.hasShardKey(toupdate));

                key = sk.extractKey(toupdate);

                BSONForEach(field, query){
                    if (!sk.partOfShardKey(field.fieldName()) || getGtLtOp(field) != BSONObj::Equality)
                        continue;
                    uassert(8014, str::stream() << "cannot modify shard key for collection: " << manager->getns(),
                            field == key[field.fieldName()]);
                }

            }
            
            const int LEFT_START = 5;
            int left = LEFT_START;
            while ( true ) {
                try {
                    Shard shard;
                    ChunkPtr c;
                    if ( key.isEmpty() ) {
                        uassert(8012, "can't upsert something without full valid shard key", !upsert);

                        set<Shard> shards;
                        manager->getShardsForQuery(shards, query);
                        if (shards.size() == 1) {
                            shard = *shards.begin();
                        }
                        else{
                            // data could be on more than one shard. must send to all
                            little<int> * x = &little<int>::ref( const_cast<char*>( r.d().afterNS() ) );
                            x[0] |= UpdateOption_Broadcast; // this means don't check shard version in mongod
                            broadcastWrite(dbUpdate, r);
                            return;
                        }
                    }
                    else {
                        uassert(16066, "", sk.hasShardKey(key));
                        c = manager->findChunk( key );
                        shard = c->getShard();
                    }

                    verify(shard != Shard());
                    doWrite( dbUpdate , r , shard );

                    if ( c &&r.getClientInfo()->autoSplitOk() )
                        c->splitIfShould( d.msg().header()->dataLen() );

                    return;
                }
                catch ( StaleConfigException& e ) {
                    if ( left <= 0 )
                        throw e;
                    log( left == LEFT_START ) << "update will be retried b/c sharding config info is stale, "
                                              << " left:" << left - 1 << " ns: " << r.getns() << " query: " << query << endl;
                    left--;
                    r.reset();
                    manager = r.getChunkManager();
                    uassert(14806, "collection no longer sharded", manager);
                }
            }
        }

        void _delete( Request& r , DbMessage& d, ChunkManagerPtr manager ) {

            int flags = d.pullInt();
            bool justOne = flags & 1;

            uassert( 10203 ,  "bad delete message" , d.moreJSObjs() );
            BSONObj pattern = d.nextJsObj();
            uassert( 13505 ,  "$atomic not supported sharded" , pattern["$atomic"].eoo() );
            
            const int LEFT_START = 5;
            int left = LEFT_START;
            while ( true ) {
                try {
                    set<Shard> shards;
                    manager->getShardsForQuery( shards , pattern );
                    LOG(2) << "delete : " << pattern << " \t " << shards.size() << " justOne: " << justOne << endl;

                    if ( shards.size() != 1 ) {
                        // data could be on more than one shard. must send to all
                        if ( justOne && ! pattern.hasField( "_id" ) )
                            throw UserException( 8015 , "can only delete with a non-shard key pattern if can delete as many as we find" );

                        little<int>* x = &little<int>::ref( const_cast<char*>( r.d().afterNS() ) );
                        x[0] |= RemoveOption_Broadcast; // this means don't check shard version in mongod
                        broadcastWrite(dbUpdate, r);
                        return;
                    }

                    doWrite( dbDelete , r , *shards.begin() );
                    return;
                }
                catch ( StaleConfigException& e ) {
                    if ( left <= 0 )
                        throw e;
                    log( left == LEFT_START ) << "delete will be retried b/c of StaleConfigException, "
                                              << " left:" << left - 1 << " ns: " << r.getns() << " patt: " << pattern << endl;
                    left--;
                    r.reset();
                    manager = r.getChunkManager();
                    uassert(14805, "collection no longer sharded", manager);
                }
            }
        }
        
        virtual void writeOp( int op , Request& r ) {

            ChunkManagerPtr info;
            ShardPtr primary;

            r.getConfig()->getChunkManagerOrPrimary( r.getns(), info, primary );

            if( primary ){

                const char *ns = r.getns();

                if ( r.isShardingEnabled() &&
                        strstr( ns , ".system.indexes" ) == strchr( ns , '.' ) &&
                        strchr( ns , '.' ) )
                {
                    LOG(1) << " .system.indexes write for: " << ns << endl;
                    handleIndexWrite( op , r );
                    return;
                }

                LOG(3) << "single write: " << ns << endl;
                SINGLE->doWrite( op , r , *primary );
                r.gotInsert(); // Won't handle mulit-insert correctly. Not worth parsing the request.

                return;
            }
            else{
                const char *ns = r.getns();
                LOG(3) << "write: " << ns << endl;

                DbMessage& d = r.d();

                if ( op == dbInsert ) {
                    _insert( r , d , info );
                }
                else if ( op == dbUpdate ) {
                    _update( r , d , info );
                }
                else if ( op == dbDelete ) {
                    _delete( r , d , info );
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
                throw UserException( 8050 , "can't update system.indexes" );
            }
            else if ( op == dbDelete ) {
                // TODO
                throw UserException( 8051 , "can't delete indexes on sharded collection yet" );
            }
            else {
                log() << "handleIndexWrite invalid write op: " << op << endl;
                throw UserException( 8052 , "handleIndexWrite invalid write op" );
            }

        }

    };

    Strategy * SHARDED = new ShardStrategy();
}
