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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/base/status.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/max_time.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/client_info.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/cursors.h"
#include "mongo/s/dbclient_shard_resolver.h"
#include "mongo/s/dbclient_multi_command.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/version_manager.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batch_upconvert.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

// error codes 8010-8040

namespace mongo {

    using boost::scoped_ptr;

    static bool _isSystemIndexes( const char* ns ) {
        return nsToCollectionSubstring(ns) == "system.indexes";
    }

    /**
     * Returns true if request is a query for sharded indexes.
     */
    static bool doShardedIndexQuery( Request& r, const QuerySpec& qSpec ) {

        // Extract the ns field from the query, which may be embedded within the "query" or
        // "$query" field.
        string indexNSQuery(qSpec.filter()["ns"].str());
        DBConfigPtr config = grid.getDBConfig( r.getns() );

        if ( !config->isSharded( indexNSQuery )) {
            return false;
        }

        // if you are querying on system.indexes, we need to make sure we go to a shard
        // that actually has chunks. This is not a perfect solution (what if you just
        // look at all indexes), but better than doing nothing.

        ShardPtr shard;
        ChunkManagerPtr cm;
        config->getChunkManagerOrPrimary( indexNSQuery, cm, shard );
        if ( cm ) {
            set<Shard> shards;
            cm->getAllShards( shards );
            verify( shards.size() > 0 );
            shard.reset( new Shard( *shards.begin() ) );
        }

        ShardConnection dbcon( *shard , r.getns() );
        DBClientBase &c = dbcon.conn();

        string actualServer;

        Message response;
        bool ok = c.call( r.m(), response, true , &actualServer );
        uassert( 10200 , "mongos: error calling db", ok );

        {
            QueryResult::View qr = response.singleData().view2ptr();
            if ( qr.getResultFlags() & ResultFlag_ShardConfigStale ) {
                dbcon.done();
                // Version is zero b/c this is deprecated codepath
                throw RecvStaleConfigException( r.getns(),
                                                "Strategy::doQuery",
                                                ChunkVersion( 0, 0, OID() ),
                                                ChunkVersion( 0, 0, OID() ));
            }
        }

        r.reply( response , actualServer.size() ? actualServer : c.getServerAddress() );
        dbcon.done();

        return true;
    }

    void Strategy::queryOp( Request& r ) {

        verify( !NamespaceString( r.getns() ).isCommand() );

        Timer queryTimer;

        QueryMessage q( r.d() );

        NamespaceString ns(q.ns);
        ClientBasic* client = ClientBasic::getCurrent();
        AuthorizationSession* authSession = client->getAuthorizationSession();
        Status status = authSession->checkAuthForQuery(ns, q.query);
        audit::logQueryAuthzCheck(client, ns, q.query, status.code());
        uassertStatusOK(status);

        LOG(3) << "query: " << q.ns << " " << q.query << " ntoreturn: " << q.ntoreturn
               << " options: " << q.queryOptions << endl;

        if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
            throw UserException( 8010 , "something is wrong, shouldn't see a command here" );

        if (q.queryOptions & QueryOption_Exhaust) {
            uasserted(18526,
                      string("the 'exhaust' query option is invalid for mongos queries: ") + q.ns
                      + " " + q.query.toString());
        }

        QuerySpec qSpec( (string)q.ns, q.query, q.fields, q.ntoskip, q.ntoreturn, q.queryOptions );

        // Parse "$maxTimeMS".
        StatusWith<int> maxTimeMS = LiteParsedQuery::parseMaxTimeMSQuery( q.query );
        uassert( 17233,
                 maxTimeMS.getStatus().reason(),
                 maxTimeMS.isOK() );

        if ( _isSystemIndexes( q.ns ) && doShardedIndexQuery( r, qSpec )) {
            return;
        }

        ParallelSortClusteredCursor * cursor = new ParallelSortClusteredCursor( qSpec, CommandInfo() );
        verify( cursor );

        // TODO:  Move out to Request itself, not strategy based
        try {
            cursor->init();

            if ( qSpec.isExplain() ) {
                BSONObjBuilder explain_builder;
                cursor->explain( explain_builder );
                explain_builder.appendNumber( "executionTimeMillis",
                                              static_cast<long long>(queryTimer.millis()) );
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

        // TODO: Revisit all of this when we revisit the sharded cursor cache

        if (cursor->getNumQueryShards() != 1) {

            // More than one shard (or zero), manage with a ShardedClientCursor
            // NOTE: We may also have *zero* shards here when the returnPartial flag is set.
            // Currently the code in ShardedClientCursor handles this.

            ShardedClientCursorPtr cc (new ShardedClientCursor( q , cursor ));

            BufBuilder buffer( ShardedClientCursor::INIT_REPLY_BUFFER_SIZE );
            int docCount = 0;
            const int startFrom = cc->getTotalSent();
            bool hasMore = cc->sendNextBatch( r, q.ntoreturn, buffer, docCount );

            if ( hasMore ) {
                LOG(5) << "storing cursor : " << cc->getId() << endl;

                int cursorLeftoverMillis = maxTimeMS.getValue() - queryTimer.millis();
                if ( maxTimeMS.getValue() == 0 ) { // 0 represents "no limit".
                    cursorLeftoverMillis = kMaxTimeCursorNoTimeLimit;
                }
                else if ( cursorLeftoverMillis <= 0 ) {
                    cursorLeftoverMillis = kMaxTimeCursorTimeLimitExpired;
                }

                cursorCache.store( cc, cursorLeftoverMillis );
            }

            replyToQuery( 0, r.p(), r.m(), buffer.buf(), buffer.len(), docCount,
                    startFrom, hasMore ? cc->getId() : 0 );
        }
        else{

            // Only one shard is used

            // Remote cursors are stored remotely, we shouldn't need this around.
            scoped_ptr<ParallelSortClusteredCursor> cursorDeleter( cursor );

            ShardPtr shard = cursor->getQueryShard();
            verify( shard.get() );
            DBClientCursorPtr shardCursor = cursor->getShardCursor(*shard);

            // Implicitly stores the cursor in the cache
            r.reply( *(shardCursor->getMessage()) , shardCursor->originalHost() );

            // We don't want to kill the cursor remotely if there's still data left
            shardCursor->decouple();
        }
    }

    void Strategy::clientCommandOp( Request& r ) {
        QueryMessage q( r.d() );

        LOG(3) << "command: " << q.ns << " " << q.query << " ntoreturn: " << q.ntoreturn
               << " options: " << q.queryOptions << endl;

        if (q.queryOptions & QueryOption_Exhaust) {
            uasserted(18527,
                      string("the 'exhaust' query option is invalid for mongos commands: ") + q.ns
                      + " " + q.query.toString());
        }

        NamespaceString nss( r.getns() );
        // Regular queries are handled in strategy_shard.cpp
        verify( nss.isCommand() || nss.isSpecialCommand() );

        if ( handleSpecialNamespaces( r , q ) )
            return;

        int loops = 5;
        while ( true ) {
            BSONObjBuilder builder;
            try {
                BSONObj cmdObj = q.query;
                {
                    BSONElement e = cmdObj.firstElement();
                    if (e.type() == Object && (e.fieldName()[0] == '$'
                                                 ? str::equals("query", e.fieldName()+1)
                                                 : str::equals("query", e.fieldName()))) {
                        // Extract the embedded query object.

                        if (cmdObj.hasField(Query::ReadPrefField.name())) {
                            // The command has a read preference setting. We don't want
                            // to lose this information so we copy this to a new field
                            // called $queryOptions.$readPreference
                            BSONObjBuilder finalCmdObjBuilder;
                            finalCmdObjBuilder.appendElements(e.embeddedObject());

                            BSONObjBuilder queryOptionsBuilder(
                                    finalCmdObjBuilder.subobjStart("$queryOptions"));
                            queryOptionsBuilder.append(cmdObj[Query::ReadPrefField.name()]);
                            queryOptionsBuilder.done();

                            cmdObj = finalCmdObjBuilder.obj();
                        }
                        else {
                            cmdObj = e.embeddedObject();
                        }
                    }
                }

                Command::runAgainstRegistered(q.ns, cmdObj, builder, q.queryOptions);
                BSONObj x = builder.done();
                replyToQuery(0, r.p(), r.m(), x);
                return;
            }
            catch ( StaleConfigException& e ) {
                if ( loops <= 0 )
                    throw e;

                loops--;
                log() << "retrying command: " << q.query << endl;

                // For legacy reasons, ns may not actually be set in the exception :-(
                string staleNS = e.getns();
                if( staleNS.size() == 0 ) staleNS = q.ns;

                ShardConnection::checkMyConnectionVersions( staleNS );
                if( loops < 4 ) versionManager.forceRemoteCheckShardVersionCB( staleNS );
            }
            catch ( AssertionException& e ) {
                Command::appendCommandStatus(builder, e.toStatus());
                BSONObj x = builder.done();
                replyToQuery(0, r.p(), r.m(), x);
                return;
            }
        }
    }

    bool Strategy::handleSpecialNamespaces( Request& r , QueryMessage& q ) {
        const char * ns = strstr( r.getns() , ".$cmd.sys." );
        if ( ! ns )
            return false;
        ns += 10;

        BSONObjBuilder b;
        vector<Shard> shards;

        ClientBasic* client = ClientBasic::getCurrent();
        AuthorizationSession* authSession = client->getAuthorizationSession();
        if ( strcmp( ns , "inprog" ) == 0 ) {
            const bool isAuthorized = authSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(), ActionType::inprog);
            audit::logInProgAuthzCheck(
                    client, q.query, isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
            uassert(ErrorCodes::Unauthorized, "not authorized to run inprog", isAuthorized);

            Shard::getAllShards( shards );

            BSONArrayBuilder arr( b.subarrayStart( "inprog" ) );

            for ( unsigned i=0; i<shards.size(); i++ ) {
                Shard shard = shards[i];
                ScopedDbConnection conn(shard.getConnString());
                BSONObj temp = conn->findOne( r.getns() , q.query );
                if ( temp["inprog"].isABSONObj() ) {
                    BSONObjIterator i( temp["inprog"].Obj() );
                    while ( i.more() ) {
                        BSONObjBuilder x;

                        BSONObjIterator j( i.next().Obj() );
                        while( j.more() ) {
                            BSONElement e = j.next();
                            if ( str::equals( e.fieldName() , "opid" ) ) {
                                stringstream ss;
                                ss << shard.getName() << ':' << e.numberInt();
                                x.append( "opid" , ss.str() );
                            }
                            else if ( str::equals( e.fieldName() , "client" ) ) {
                                x.appendAs( e , "client_s" );
                            }
                            else {
                                x.append( e );
                            }
                        }
                        arr.append( x.obj() );
                    }
                }
                conn.done();
            }

            arr.done();
        }
        else if ( strcmp( ns , "killop" ) == 0 ) {
            const bool isAuthorized = authSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(), ActionType::killop);
            audit::logKillOpAuthzCheck(
                    client,
                    q.query,
                    isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
            uassert(ErrorCodes::Unauthorized, "not authorized to run killop", isAuthorized);

            BSONElement e = q.query["op"];
            if ( e.type() != String ) {
                b.append( "err" , "bad op" );
                b.append( e );
            }
            else {
                b.append( e );
                string s = e.String();
                string::size_type i = s.find( ':' );
                if ( i == string::npos ) {
                    b.append( "err" , "bad opid" );
                }
                else {
                    string shard = s.substr( 0 , i );
                    int opid = atoi( s.substr( i + 1 ).c_str() );
                    b.append( "shard" , shard );
                    b.append( "shardid" , opid );

                    log() << "want to kill op: " << e << endl;
                    Shard s(shard);

                    ScopedDbConnection conn(s.getConnString());
                    conn->findOne( r.getns() , BSON( "op" << opid ) );
                    conn.done();
                }
            }
        }
        else if ( strcmp( ns , "unlock" ) == 0 ) {
            b.append( "err" , "can't do unlock through mongos" );
        }
        else {
            warning() << "unknown sys command [" << ns << "]" << endl;
            return false;
        }

        BSONObj x = b.done();
        replyToQuery(0, r.p(), r.m(), x);
        return true;
    }

    void Strategy::commandOp( const string& db,
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

    Status Strategy::commandOpWrite(const std::string& dbName,
                                    const BSONObj& command,
                                    BatchItemRef targetingBatchItem,
                                    std::vector<CommandResult>* results) {

        // Note that this implementation will not handle targeting retries and does not completely
        // emulate write behavior

        ChunkManagerTargeter targeter;
        Status status =
            targeter.init(NamespaceString(targetingBatchItem.getRequest()->getTargetingNS()));
        if (!status.isOK())
            return status;

        OwnedPointerVector<ShardEndpoint> endpointsOwned;
        vector<ShardEndpoint*>& endpoints = endpointsOwned.mutableVector();

        if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Insert) {
            ShardEndpoint* endpoint;
            Status status = targeter.targetInsert(targetingBatchItem.getDocument(), &endpoint);
            if (!status.isOK())
                return status;
            endpoints.push_back(endpoint);
        }
        else if (targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
            Status status = targeter.targetUpdate(*targetingBatchItem.getUpdate(), &endpoints);
            if (!status.isOK())
                return status;
        }
        else {
            invariant(targetingBatchItem.getOpType() == BatchedCommandRequest::BatchType_Delete);
            Status status = targeter.targetDelete(*targetingBatchItem.getDelete(), &endpoints);
            if (!status.isOK())
                return status;
        }

        DBClientShardResolver resolver;
        DBClientMultiCommand dispatcher;

        // Assemble requests
        for (vector<ShardEndpoint*>::const_iterator it = endpoints.begin(); it != endpoints.end();
            ++it) {

            const ShardEndpoint* endpoint = *it;

            ConnectionString host;
            Status status = resolver.chooseWriteHost(endpoint->shardName, &host);
            if (!status.isOK())
                return status;

            RawBSONSerializable request(command);
            dispatcher.addCommand(host, dbName, request);
        }

        // Errors reported when recv'ing responses
        dispatcher.sendAll();
        Status dispatchStatus = Status::OK();

        // Recv responses
        while (dispatcher.numPending() > 0) {

            ConnectionString host;
            RawBSONSerializable response;

            Status status = dispatcher.recvAny(&host, &response);
            if (!status.isOK()) {
                // We always need to recv() all the sent operations
                dispatchStatus = status;
                continue;
            }

            CommandResult result;
            result.target = host;
            result.shardTarget = Shard::make(host.toString());
            result.result = response.toBSON();

            results->push_back(result);
        }

        return dispatchStatus;
    }

    Status Strategy::commandOpUnsharded(const std::string& db,
                                        const BSONObj& command,
                                        int options,
                                        const std::string& versionedNS,
                                        CommandResult* cmdResult) {

        // Note that this implementation will not handle targeting retries and fails when the
        // sharding metadata is too stale

        DBConfigPtr conf = grid.getDBConfig(db , false);
        if (!conf) {
            mongoutils::str::stream ss;
            ss << "Passthrough command failed: " << command.toString()
               << " on ns " << versionedNS << ". Cannot find db config info.";
            return Status(ErrorCodes::IllegalOperation, ss);
        }

        if (conf->isSharded(versionedNS)) {
            mongoutils::str::stream ss;
            ss << "Passthrough command failed: " << command.toString()
               << " on ns " << versionedNS << ". Cannot run on sharded namespace.";
            return Status(ErrorCodes::IllegalOperation, ss);
        }

        Shard primaryShard = conf->getPrimary();

        BSONObj shardResult;
        try {
            ShardConnection conn(primaryShard, "");
            // TODO: this can throw a stale config when mongos is not up-to-date -- fix.
            conn->runCommand(db, command, shardResult, options);
            conn.done();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }

        // Fill out the command result.
        cmdResult->shardTarget = primaryShard;
        cmdResult->result = shardResult;
        cmdResult->target = primaryShard.getAddress();

        return Status::OK();
    }

    void Strategy::getMore( Request& r ) {

        Timer getMoreTimer;

        const char *ns = r.getns();

        // TODO:  Handle stale config exceptions here from coll being dropped or sharded during op
        // for now has same semantics as legacy request
        DBConfigPtr config = grid.getDBConfig( ns );
        ShardPtr primary;
        ChunkManagerPtr info;
        config->getChunkManagerOrPrimary( ns, info, primary );

        //
        // TODO: Cleanup cursor cache, consolidate into single codepath
        //

        int ntoreturn = r.d().pullInt();
        long long id = r.d().pullInt64();
        string host = cursorCache.getRef( id );
        ShardedClientCursorPtr cursor = cursorCache.get( id );
        int cursorMaxTimeMS = cursorCache.getMaxTimeMS( id );

        // Cursor ids should not overlap between sharded and unsharded cursors
        massert( 17012, str::stream() << "duplicate sharded and unsharded cursor id "
                                      << id << " detected for " << ns
                                      << ", duplicated on host " << host,
                 NULL == cursorCache.get( id ).get() || host.empty() );

        ClientBasic* client = ClientBasic::getCurrent();
        NamespaceString nsString(ns);
        AuthorizationSession* authSession = client->getAuthorizationSession();
        Status status = authSession->checkAuthForGetMore( nsString, id );
        audit::logGetMoreAuthzCheck( client, nsString, id, status.code() );
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

            bool hasMore = (response.singleData().getCursor() != 0);

            if ( !hasMore ) {
                cursorCache.removeRef( id );
            }

            r.reply( response , "" /*conn->getServerAddress() */ );
            conn.done();
            return;
        }
        else if ( cursor ) {

            if ( cursorMaxTimeMS == kMaxTimeCursorTimeLimitExpired ) {
                cursorCache.remove( id );
                uasserted( ErrorCodes::ExceededTimeLimit, "operation exceeded time limit" );
            }

            // TODO: Try to match logic of mongod, where on subsequent getMore() we pull lots more data?
            BufBuilder buffer( ShardedClientCursor::INIT_REPLY_BUFFER_SIZE );
            int docCount = 0;
            const int startFrom = cursor->getTotalSent();
            bool hasMore = cursor->sendNextBatch( r, ntoreturn, buffer, docCount );

            if ( hasMore ) {
                // still more data
                cursor->accessed();

                if ( cursorMaxTimeMS != kMaxTimeCursorNoTimeLimit ) {
                    // Update remaining amount of time in cursor cache.
                    int cursorLeftoverMillis = cursorMaxTimeMS - getMoreTimer.millis();
                    if ( cursorLeftoverMillis <= 0 ) {
                        cursorLeftoverMillis = kMaxTimeCursorTimeLimitExpired;
                    }
                    cursorCache.updateMaxTimeMS( id, cursorLeftoverMillis );
                }
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

    void Strategy::writeOp( int op , Request& r ) {

        // make sure we have a last error
        dassert( lastError.get( false /* don't create */) );

        OwnedPointerVector<BatchedCommandRequest> requestsOwned;
        vector<BatchedCommandRequest*>& requests = requestsOwned.mutableVector();

        msgToBatchRequests( r.m(), &requests );

        for ( vector<BatchedCommandRequest*>::iterator it = requests.begin();
            it != requests.end(); ++it ) {

            // Multiple commands registered to last error as multiple requests
            if ( it != requests.begin() )
                lastError.startRequest( r.m(), lastError.get( false ) );

            BatchedCommandRequest* request = *it;

            // Adjust namespaces for command
            NamespaceString fullNS( request->getNS() );
            string cmdNS = fullNS.getCommandNS();
            // We only pass in collection name to command
            request->setNS( fullNS.coll() );

            BSONObjBuilder builder;
            BSONObj requestBSON = request->toBSON();

            {
                // Disable the last error object for the duration of the write cmd
                LastError::Disabled disableLastError( lastError.get( false ) );
                Command::runAgainstRegistered( cmdNS.c_str(), requestBSON, builder, 0 );
            }

            BatchedCommandResponse response;
            bool parsed = response.parseBSON( builder.done(), NULL );
            (void) parsed; // for compile
            dassert( parsed && response.isValid( NULL ) );

            // Populate the lastError object based on the write response
            lastError.get( false )->reset();
            bool hadError = batchErrorToLastError( *request, response, lastError.get( false ) );

            // Check if this is an ordered batch and we had an error which should stop processing
            if ( request->getOrdered() && hadError )
                break;
        }
    }

    Strategy * STRATEGY = new Strategy();
}
