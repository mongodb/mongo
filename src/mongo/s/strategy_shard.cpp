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

// strategy_sharded.cpp

#include "mongo/pch.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/index.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/client_info.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/version_manager.h"
#include "mongo/util/mongoutils/str.h"

// error codes 8010-8040

namespace mongo {

    class ShardStrategy : public Strategy {

        bool _isSystemIndexes( const char* ns ) {
            return nsToCollectionSubstring(ns) == "system.indexes";
        }

        virtual void queryOp( Request& r ) {

            // Commands are handled in strategy_single.cpp
            verify( !NamespaceString( r.getns() ).isCommand() );

            QueryMessage q( r.d() );

            ClientBasic* client = ClientBasic::getCurrent();
            AuthorizationSession* authSession = client->getAuthorizationSession();
            Status status = authSession->checkAuthForQuery(q.ns, q.query);
            audit::logQueryAuthzCheck(client, NamespaceString(q.ns), q.query, status.code());
            uassertStatusOK(status);

            LOG(3) << "shard query: " << q.ns << "  " << q.query << endl;

            if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
                throw UserException( 8010 , "something is wrong, shouldn't see a command here" );

            QuerySpec qSpec( (string)q.ns, q.query, q.fields, q.ntoskip, q.ntoreturn, q.queryOptions );

            if ( _isSystemIndexes( q.ns ) && q.query["ns"].type() == String && r.getConfig()->isSharded( q.query["ns"].String() ) ) {
                // if you are querying on system.indexes, we need to make sure we go to a shard that actually has chunks
                // this is not a perfect solution (what if you just look at all indexes)
                // but better than doing nothing
                
                ShardPtr myShard;
                ChunkManagerPtr cm;
                r.getConfig()->getChunkManagerOrPrimary( q.query["ns"].String(), cm, myShard );
                if ( cm ) {
                    set<Shard> shards;
                    cm->getAllShards( shards );
                    verify( shards.size() > 0 );
                    myShard.reset( new Shard( *shards.begin() ) );
                }
                
                doIndexQuery( r, *myShard );
                return;
            }
            
            ParallelSortClusteredCursor * cursor = new ParallelSortClusteredCursor( qSpec, CommandInfo() );
            verify( cursor );

            // TODO:  Move out to Request itself, not strategy based
            try {
                long long start_millis = 0;
                if ( qSpec.isExplain() ) start_millis = curTimeMillis64();
                cursor->init();

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
                // Remote cursors are stored remotely, we shouldn't need this around.
                // TODO: we should probably just make cursor an auto_ptr
                scoped_ptr<ParallelSortClusteredCursor> cursorDeleter( cursor );

                // TODO:  Better merge this logic.  We potentially can now use the same cursor logic for everything.
                ShardPtr primary = cursor->getPrimary();
                verify( primary.get() );
                DBClientCursorPtr shardCursor = cursor->getShardCursor( *primary );

                // Implicitly stores the cursor in the cache
                r.reply( *(shardCursor->getMessage()) , shardCursor->originalHost() );

                // We don't want to kill the cursor remotely if there's still data left
                shardCursor->decouple();
            }
        }

        virtual void commandOp( const string& db,
                                const BSONObj& command,
                                int options,
                                const string& versionedNS,
                                const BSONObj& targetingQuery,
                                vector<CommandResult>* results )
        {

            QuerySpec qSpec(db + ".$cmd", command, BSONObj(), 0, 1, options);

            ParallelSortClusteredCursor cursor( qSpec, CommandInfo( versionedNS, targetingQuery ) );

            // Initialize the cursor
            cursor.init();

            set<Shard> shards;
            cursor.getQueryShards( shards );

            for( set<Shard>::iterator i = shards.begin(), end = shards.end(); i != end; ++i ){
                CommandResult result;
                result.shardTarget = *i;
                string errMsg; // ignored, should never be invalid b/c an exception thrown earlier
                result.target =
                        ConnectionString::parse( cursor.getShardCursor( *i )->originalHost(),
                                                 errMsg );
                result.result = cursor.getShardCursor( *i )->peekFirst().getOwned();
                results->push_back( result );
            }

        }

        virtual void getMore( Request& r ) {

            const char *ns = r.getns();

            // TODO:  Handle stale config exceptions here from coll being dropped or sharded during op
            // for now has same semantics as legacy request
            ChunkManagerPtr info = r.getChunkManager();

            //
            // TODO: Cleanup cursor cache, consolidate into single codepath
            //

            int ntoreturn = r.d().pullInt();
            long long id = r.d().pullInt64();
            string host = cursorCache.getRef( id );
            ShardedClientCursorPtr cursor = cursorCache.get( id );

            // Cursor ids should not overlap between sharded and unsharded cursors
            massert( 17012, str::stream() << "duplicate sharded and unsharded cursor id "
                                          << id << " detected for " << ns
                                          << ", duplicated on host " << host,
                     NULL == cursorCache.get( id ).get() || host.empty() );

            ClientBasic* client = ClientBasic::getCurrent();
            AuthorizationSession* authSession = client->getAuthorizationSession();
            Status status = authSession->checkAuthForGetMore( ns, id );
            audit::logGetMoreAuthzCheck( client, NamespaceString(ns), id, status.code() );
            uassertStatusOK(status);

            if( !host.empty() ){

                LOG(3) << "single getmore: " << ns << endl;

                // we used ScopedDbConnection because we don't get about config versions
                // not deleting data is handled elsewhere
                // and we don't want to call setShardVersion
                ScopedDbConnection conn(host);

                Message response;
                bool ok = conn->callRead( r.m() , response);
                uassert( 10204 , "dbgrid: getmore: error calling db", ok);

                bool hasMore = (response.singleData()->getCursor() != 0);

                if ( !hasMore ) {
                    cursorCache.removeRef( id );
                }

                r.reply( response , "" /*conn->getServerAddress() */ );
                conn.done();
                return;
            }
            else if ( cursor ) {

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
                return;
            }
            else {

                LOG( 3 ) << "could not find cursor " << id << " in cache for " << ns << endl;

                replyToQuery( ResultFlag_CursorNotFound , r.p() , r.m() , 0 , 0 , 0 );
                return;
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
        void _sleepForVerifiedLocalError() {

            if (!perThreadBackoff.get()) perThreadBackoff.reset(new Backoff(maxWaitMillis,
                                                                            maxWaitMillis * 2));

            perThreadBackoff.get()->nextSleepMillis();
        }

        void _handleRetries(const string& op,
                            int retries,
                            const string& ns,
                            const BSONObj& query,
                            StaleConfigException& e,
                            Request& r) // TODO: remove
        {
            static const int MAX_RETRIES = 5;
            if (retries >= MAX_RETRIES) {
                // If we rethrow b/c too many retries, make sure we add as much data as possible
                e.addContext(query.toString());
                throw e;
            }

            //
            // On a stale config exception, we have to assume that the entire collection could have
            // become unsharded, or sharded with a different shard key - we need to re-run all the
            // targeting we've done earlier
            //

            LOG( retries == 0 ? 1 : 0 ) << op << " will be retried b/c sharding config info is stale, "
                              << " retries: " << retries
                              << " ns: " << ns
                              << " data: " << query << endl;

            if (retries > 2) {
                versionManager.forceRemoteCheckShardVersionCB(ns);
            }

            r.reset();
        }

        struct InsertGroup {

            InsertGroup() :
                    reloadedConfig(false)
            {
            }

            // Does NOT reset our config reload flag
            void resetInsertData() {
                shard.reset();
                manager.reset();
                inserts.clear();
                chunkData.clear();
                errMsg.clear();
            }

            bool hasException() {
                return errMsg != "";
            }

            void setException(int code, const string& errMsg) {
                this->errCode = code;
                this->errMsg = errMsg;
            }

            ShardPtr shard;
            ChunkManagerPtr manager;
            vector<BSONObj> inserts;
            map<ChunkPtr, int> chunkData;
            bool reloadedConfig;

            string errMsg;
            int errCode;

        };

        /**
         * Given a ns and insert message (with flags), returns the shard (and chunkmanager if
         * needed) required to do a bulk insert of the next N inserts until the shard changes.
         *
         * Also tracks the data inserted per chunk on the current shard.
         *
         * Returns whether or not the config data was reloaded
         */
        void _getNextInsertGroup(const string& ns, DbMessage& d, int flags, InsertGroup* group) {
            grid.getDBConfig(ns)->getChunkManagerOrPrimary(ns, group->manager, group->shard);
            // shard is either primary or nothing, if there's a chunk manager

            // Set our current position, so we can jump back if we have a stale config error for
            // this group
            d.markSet();

            int totalInsertSize = 0;

            while (d.moreJSObjs()) {

                const char* prevObjMark = d.markGet();
                BSONObj o = d.nextJsObj();

                if (group->manager && !group->manager->hasShardKey(o)) {

                    bool bad = true;

                    // If _id is part of shard key pattern, but item doesn't already have one,
                    // add autogenerated _id and see if we now have a shard key.
                    if (group->manager->getShardKey().partOfShardKey("_id") && !o.hasField("_id")) {

                        BSONObjBuilder b;
                        b.appendOID("_id", 0, true);
                        b.appendElements(o);
                        o = b.obj();
                        bad = !group->manager->hasShardKey(o);

                    }

                    if (bad && !group->reloadedConfig) {

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
                                  << ", expected values for " << group->manager->getShardKey()
                                  << ", reloading config data to ensure not stale" << endl;

                        // Reset the selected shard and manager
                        group->shard.reset();
                        group->manager.reset();
                        // Remove all the previously grouped inserts...
                        group->inserts.clear();
                        // Reset the chunk data...
                        group->chunkData.clear();

                        // If this is our retry, force talking to the config server
                        grid.getDBConfig(ns)->getChunkManagerIfExists(ns, true);

                        // Now we've reloaded the config once
                        group->reloadedConfig = true;

                        // Reset our current position to the start of the last group of inserts
                        d.markReset();

                        _getNextInsertGroup(ns, d, flags, group);
                        return;
                    }

                    if (bad) {

                        // Sleep to avoid DOS'ing config server when we have invalid inserts
                        _sleepForVerifiedLocalError();

                        // TODO: Matching old behavior, but do we need this log line?
                        log() << "tried to insert object with no valid shard key for "
                              << group->manager->getShardKey() << " : " << o << endl;

                        group->setException(8011,
                                            str::stream()
                                                    << "tried to insert object with no valid shard key for "
                                                    << group->manager->getShardKey().toString()
                                                    << " : " << o.toString());
                        return;
                    }
                }

                int objSize = o.objsize();
                totalInsertSize += objSize;

                // Insert at least one document, but otherwise no more than 8MB of data, otherwise
                // the WBL will not work
                if (group->inserts.size() > 0 && totalInsertSize > BSONObjMaxUserSize / 2) {
                    // Reset to after the previous insert
                    d.markReset(prevObjMark);

                    LOG(3) << "breaking up bulk insert group to " << ns << " at size "
                               << (totalInsertSize - objSize) << " (" << group->inserts.size()
                               << " documents)" << endl;

                    // Too much data would be inserted, break out of our bulk insert loop
                    break;
                }

                // Make sure our objSize is not greater than maximum, otherwise WBL won't work
                verify( objSize <= BSONObjMaxUserSize );

                // Many operations benefit from having the shard key early in the object
                if (group->manager) {

                    //
                    // Sharded insert
                    //

                    ChunkPtr chunk = group->manager->findChunkForDoc(o);

                    if (!group->shard) {
                        group->shard.reset(new Shard(chunk->getShard()));
                    }
                    else if (group->shard->getName() != chunk->getShard().getName()) {

                        // Reset to after the previous insert
                        d.markReset(prevObjMark);
                        // Our shard has changed, break out of bulk insert loop
                        break;
                    }

                    o = group->manager->getShardKey().moveToFront(o);
                    group->inserts.push_back(o);
                    group->chunkData[chunk] += objSize;
                }
                else {

                    //
                    // Unsharded insert
                    //

                    group->inserts.push_back(o);
                }
            }
        }

        /**
         * This insert function now handes all inserts, unsharded or sharded, through mongos.
         *
         * Semantics for insert are ContinueOnError - to match mongod semantics :
         * 1) Error is thrown immediately for corrupt objects
         * 2) Error is thrown only for UserExceptions during the insert process, if last obj had error that's thrown
         */
        void _insert(Request& r, DbMessage& d) {

            const string& ns = r.getns();

            int flags = 0;

            if (d.reservedField() & Reserved_InsertOption_ContinueOnError) flags |=
                    InsertOption_ContinueOnError;

            if (d.reservedField() & Reserved_FromWriteback) flags |= WriteOption_FromWriteback;

            if (!d.moreJSObjs()) return;

            _insert(ns, d, flags, r);
        }

        void _insert(const string& ns, DbMessage& d, int flags, Request& r) // TODO: remove
        {
            uassert( 16056, str::stream() << "shutting down server during insert", ! inShutdown() );

            bool continueOnError = flags & InsertOption_ContinueOnError;

            // Sanity check, probably not needed but for safety
            int retries = 0;

            InsertGroup group;

            bool prevInsertException = false;

            while (d.moreJSObjs()) {

                // TODO: Replace this with a better check to see if we're making progress
                uassert( 16055, str::stream() << "too many retries during insert", retries < 30 );

                //
                // PREPARE INSERT
                //

                group.resetInsertData();

                // This function handles grouping the inserts per-shard whether the collection
                // is sharded or not.
                //
                // Can record errors on bad insert object format (i.e. doesn't have the shard
                // key), which should be handled in-order with mongod errors.
                //
                _getNextInsertGroup(ns, d, flags, &group);

                // We should always have a shard if we have any inserts
                verify(group.inserts.size() == 0 || group.shard.get());

                for (vector<BSONObj>::iterator it = group.inserts.begin();
                        it != group.inserts.end(); ++it) {
                    ClientBasic* client = ClientBasic::getCurrent();
                    AuthorizationSession* authSession = client->getAuthorizationSession();
                    Status status = authSession->checkAuthForInsert(ns, *it);
                    audit::logInsertAuthzCheck(client, NamespaceString(ns), *it, status.code());
                    uassertStatusOK(status);
                }

                if (group.inserts.size() > 0 && group.hasException()) {
                    warning() << "problem preparing batch insert detected, first inserting "
                              << group.inserts.size() << " intermediate documents" << endl;
                }

                scoped_ptr<ShardConnection> dbconPtr;

                try {

                    //
                    // DO ALL VALID INSERTS
                    //

                    if (group.inserts.size() > 0) {

                        dbconPtr.reset(new ShardConnection(*(group.shard), ns, group.manager));
                        ShardConnection& dbcon = *dbconPtr;

                        LOG(5)
                                << "inserting "
                                << group.inserts.size()
                                << " documents to shard "
                                << group.shard->toString()
                                << " at version "
                                << (group.manager.get() ?
                                    group.manager->getVersion().toString() :
                                    ChunkVersion(0, OID()).toString())
                                << endl;

                        //
                        // CHECK VERSION
                        //

                        // Will throw SCE if we need to reset our version before sending.
                        dbcon.setVersion();

                        // Reset our retries to zero since this batch's version went through
                        retries = 0;

                        //
                        // SEND INSERT
                        //

                        string insertErr = "";

                        try {

                            dbcon->insert(ns, group.inserts, flags);

                            //
                            // WARNING: We *have* to return the connection here, otherwise the
                            // error gets checked on a different connection!
                            //
                            dbcon.done();

                            globalOpCounters.incInsertInWriteLock(group.inserts.size());

                            //
                            // CHECK INTERMEDIATE ERROR
                            //

                            // We need to check the mongod error if we're inserting more documents,
                            // or if a later mongos error might mask an insert error,
                            // or if an earlier error might mask this error from GLE
                            if (d.moreJSObjs() || group.hasException() || prevInsertException) {

                                LOG(3) << "running intermediate GLE to "
                                       << group.shard->toString() << " during bulk insert "
                                       << "because "
                                       << (d.moreJSObjs() ? "we have more documents to insert" : 
                                          (group.hasException() ? "exception detected while preparing group" :
                                                                      "a previous error exists"))
                                       << endl;

                                ClientInfo* ci = r.getClientInfo();

                                //
                                // WARNING: Without this, we will use the *previous* shard for GLE
                                //
                                ci->newRequest();

                                BSONObjBuilder gleB;
                                string errMsg;

                                // TODO: Can't actually pass GLE parameters here,
                                // so we use defaults?
                                ci->getLastError("admin",
                                                 BSON( "getLastError" << 1 ),
                                                 gleB,
                                                 errMsg,
                                                 false);

                                insertErr = errMsg;
                                BSONObj gle = gleB.obj();
                                if (gle["err"].type() == String)
                                    insertErr = gle["err"].String();

                                LOG(3) << "intermediate GLE result was " << gle
                                       << " errmsg: " << errMsg << endl;

                                //
                                // Clear out the shards we've checked so far, if we're successful
                                //
                                ci->clearSinceLastGetError();
                            }
                        }
                        catch (DBException& e) {
                            // Network error on send or GLE
                            insertErr = e.what();
                            dbcon.kill();
                        }

                        //
                        // If the insert had an error, figure out if we should throw right now.
                        //

                        if (insertErr.size() > 0) {

                            string errMsg = str::stream()
                                    << "error inserting "
                                    << group.inserts.size()
                                    << " documents to shard "
                                    << group.shard->toString()
                                    << " at version "
                                    << (group.manager.get() ?
                                        group.manager->getVersion().toString() :
                                        ChunkVersion(0, OID()).toString())
                                    << causedBy(insertErr);

                            // If we're continuing-on-error and the insert error is superseded by
                            // a later error from mongos, we shouldn't throw this error but the
                            // later one.
                            if (group.hasException() && continueOnError) warning() << errMsg;
                            else uasserted(16460, errMsg);
                        }

                        //
                        // SPLIT CHUNKS IF NEEDED
                        //

                        // Should never throw errors!
                        if (!group.chunkData.empty() && r.getClientInfo()->autoSplitOk()) {

                            for (map<ChunkPtr, int>::iterator it = group.chunkData.begin();
                                    it != group.chunkData.end(); ++it)
                            {
                                ChunkPtr c = it->first;
                                int bytesWritten = it->second;

                                c->splitIfShould(bytesWritten);
                            }
                        }
                    }

                    //
                    // CHECK AND RE-THROW MONGOS ERROR
                    //

                    if (group.hasException()) {

                        string errMsg = str::stream() << "error preparing documents for insert"
                                                      << causedBy(group.errMsg);

                        uasserted(group.errCode, errMsg);
                    }
                }
                catch (StaleConfigException& e) {

                    // Clean up the conn if needed
                    if (dbconPtr) dbconPtr->done();

                    // Note - this can throw a SCE which will abort *all* the rest of the inserts
                    // if we retry too many times.  We assume that this cannot happen.  In any
                    // case, the user gets an error.
                    _handleRetries("insert", retries, ns, group.inserts[0], e, r);
                    retries++;

                    // Go back to the start of the inserts
                    d.markReset();

                    verify( d.moreJSObjs() );
                }
                catch (UserException& e) {

                    // Unexpected exception, cleans up the conn if not already done()
                    if (dbconPtr) dbconPtr->kill();

                    warning() << "exception during insert"
                              << (continueOnError ? " (continue on error set)" : "") << causedBy(e)
                              << endl;

                    prevInsertException = true;

                    //
                    // Throw if this is the last chunk bulk-inserted to, or if continue-on-error is
                    // not set.
                    //
                    // If this is the last chunk bulk-inserted to, then if continue-on-error is
                    // true, we'll have the final error, and if continue-on-error is false, we
                    // should abort anyway.
                    //

                    if (!d.moreJSObjs() || !continueOnError) {
                        throw;
                    }

                    //
                    // WE SWALLOW THE EXCEPTION HERE BY DESIGN
                    // to match mongod behavior
                    //
                    // TODO: Make better semantics
                    //

                    warning() << "swallowing exception during insert" << causedBy(e) << endl;
                }

                // Reset our list of last shards we talked to, since we already got writebacks
                // earlier.
                if (d.moreJSObjs()) r.getClientInfo()->clearSinceLastGetError();
            }
        }

        void _prepareUpdate(const string& ns,
                            const BSONObj& query,
                            const BSONObj& toUpdate,
                            int flags,
                            // Output
                            ChunkPtr& chunk,
                            ShardPtr& shard,
                            ChunkManagerPtr& manager,
                            ShardPtr& primary,
                            // Input
                            bool reloadConfigData = false)
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

                    if( field != toUpdate[ field.fieldName() ] ){

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

                chunk = manager->findIntersectingChunk( shardKey );
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

                if( query.hasField("$atomic") || query.hasField("$isolated") ){

                    // We can't run an atomic operation on more than one shard

                    // Retry reloading the config data once
                    if( ! reloadConfigData ){
                        _prepareUpdate( ns, query, toUpdate, flags, chunk, shard, manager, primary, true );
                        return;
                    }

                    _sleepForVerifiedLocalError();

                    uasserted( 13506, str::stream()
                        << "$atomic/$isolated not supported sharded : " << query );
                }

                return;
            }
        }

        void _update( Request& r , DbMessage& d ){

            // const details of the request
            const string& ns = r.getns();
            int flags = d.pullInt();
            const BSONObj query = d.nextJsObj();

            bool upsert = flags & UpdateOption_Upsert;

            uassert( 10201 ,  "invalid update" , d.moreJSObjs() );

            const BSONObj toUpdate = d.nextJsObj();

            ClientBasic* client = ClientBasic::getCurrent();
            AuthorizationSession* authzSession = client->getAuthorizationSession();
            Status status = authzSession->checkAuthForUpdate(ns, query, toUpdate, upsert);
            audit::logUpdateAuthzCheck(
                    client,
                    NamespaceString(ns),
                    query,
                    toUpdate,
                    upsert,
                    flags & UpdateOption_Multi,
                    status.code());
            uassertStatusOK(status);

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

            if( query.hasField( "$atomic" ) || query.hasField( "$isolated" ) ){

                // Retry reloading the config data once
                if( ! reloadConfigData ){
                    _prepareDelete( ns, query, flags, shard, manager, primary, true );
                    return;
                }

                // Sleep so we don't DOS config server
                _sleepForVerifiedLocalError();

                uasserted( 13505, str::stream()
                    << "$atomic/$isolated not supported for sharded delete : " << query );
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

            ClientBasic* client = ClientBasic::getCurrent();
            AuthorizationSession* authSession = client->getAuthorizationSession();
            Status status = authSession->checkAuthForDelete(ns, query);
            audit::logDeleteAuthzCheck(client, NamespaceString(ns), query, status.code());
            uassertStatusOK(status);

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
                broadcastWrite(dbDelete, r);
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
            bool isIndexWrite = _isSystemIndexes( ns );

            // TODO: This block goes away, system.indexes needs to handle better
            if( isIndexWrite ){

                if (op == dbInsert) {
                    // Insert is the only write op allowed on system.indexes, so it's the only one
                    // we check auth for.
                    ClientBasic* client = ClientBasic::getCurrent();
                    AuthorizationSession* authSession = client->getAuthorizationSession();
                    DbMessage& d = r.d();
                    NamespaceString nsAsNs(ns);
                    while (d.moreJSObjs()) {
                        BSONObj toInsert = d.nextJsObj();
                        Status status = authSession->checkAuthForInsert(
                                ns,
                                toInsert);
                        audit::logInsertAuthzCheck(
                                client,
                                nsAsNs,
                                toInsert,
                                status.code());
                        uassertStatusOK(status);
                    }
                    d.markReset();
                }

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
