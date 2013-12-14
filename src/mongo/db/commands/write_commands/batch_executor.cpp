/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/commands/write_commands/batch_executor.h"

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/write_ops/batched_error_detail.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    WriteBatchExecutor::WriteBatchExecutor( const BSONObj& wc,
                                            Client* client,
                                            OpCounters* opCounters,
                                            LastError* le )
        : _defaultWriteConcern(wc), _client( client ), _opCounters( opCounters ), _le( le ) {
    }

    static void maybeBuildWCError( const Status& wcStatus,
                                   const WriteConcernResult& wcResult,
                                   BatchedCommandResponse* response ) {

        // Error reported is either the errmsg or err from wc
        string errMsg;
        if ( !wcStatus.isOK() )
            errMsg = wcStatus.toString();
        else if ( wcResult.err.size() )
            errMsg = wcResult.err;

        if ( errMsg.empty() )
            return;

        if ( wcStatus.isOK() )
            response->setErrCode( ErrorCodes::WriteConcernFailed );
        else
            response->setErrCode( wcStatus.code() );

        if ( wcResult.wTimedOut )
            response->setErrInfo( BSON( "wtimeout" << true ) );

        response->setErrMessage( errMsg );
    }

    void WriteBatchExecutor::executeBatch( const BatchedCommandRequest& request,
                                           BatchedCommandResponse* response ) {

        Timer commandTimer;

        WriteStats stats;
        std::auto_ptr<BatchedErrorDetail> error( new BatchedErrorDetail );
        bool verbose = request.isVerboseWC();

        // Apply each batch item, stopping on an error if we were asked to apply the batch
        // sequentially.
        size_t numBatchItems = request.sizeWriteOps();
        size_t numItemErrors = 0;
        bool staleBatch = false;
        for ( size_t i = 0; i < numBatchItems; i++ ) {

            BSONObj upsertedID = BSONObj();
            if ( applyWriteItem( BatchItemRef( &request, i ),
                                 &stats,
                                 &upsertedID,
                                 error.get() ) ) {

                // In case updates turned out to be upserts, the callers may be interested
                // in learning what _id was used for that document.
                if ( !upsertedID.isEmpty() ) {
                    if ( numBatchItems == 1 ) {
                        response->setSingleUpserted(upsertedID);
                    }
                    else if ( verbose ) {
                        std::auto_ptr<BatchedUpsertDetail> upsertDetail(new BatchedUpsertDetail);
                        upsertDetail->setIndex(i);
                        upsertDetail->setUpsertedID(upsertedID);
                        response->addToUpsertDetails(upsertDetail.release());
                    }
                }
            }
            else {

                // The applyWriteItem did not go thgrou
                // If the error is sharding related, we'll have to investigate whether we
                // have a stale view of sharding state.
                if ( error->getErrCode() == ErrorCodes::StaleShardVersion ) staleBatch = true;

                // Don't bother recording if the user doesn't want a verbose answer. We want to
                // keep the error if this is a one-item batch, since we already compact the
                // response for those.
                if (verbose || numBatchItems == 1) {
                    error->setIndex( static_cast<int>( i ) );
                    response->addToErrDetails( error.release() );
                }

                ++numItemErrors;

                if ( request.getOrdered() ) break;

                error.reset( new BatchedErrorDetail );
            }
        }

        // So far, we may have failed some of the batch's items. So we record
        // that. Rergardless, we still need to apply the write concern.  If that generates a
        // more specific error, we'd replace for the intermediate error here. Note that we
        // "compatct" the error messge if this is an one-item batch. (See rationale later in
        // this file.)
        if ( numItemErrors > 0 ) {

            if (numBatchItems > 1) {
                // TODO
                // Define the final error code here.
                // Might be used as a final error, depending on write concern success.
                response->setErrCode( ErrorCodes::MultipleErrorsOccurred );
                response->setErrMessage( "batch op errors occurred" );
            }
            else {
                // Promote the single error.
                const BatchedErrorDetail* error = response->getErrDetailsAt( 0 );
                response->setErrCode( error->getErrCode() );
                if ( error->isErrInfoSet() ) response->setErrInfo( error->getErrInfo() );
                response->setErrMessage( error->getErrMessage() );
                response->unsetErrDetails();
                error = NULL;
            }
        }

        // Send opTime in response
        if ( anyReplEnabled() )
            response->setLastOp( _client->getLastOp().asDate() );

        // Apply write concern if we had any successful writes
        if ( numItemErrors < numBatchItems ) {

            WriteConcernOptions writeConcern;
            Status status = Status::OK();
            if ( request.isWriteConcernSet() ) {
                status = writeConcern.parse( request.getWriteConcern() );
            }
            else {
                status = writeConcern.parse( _defaultWriteConcern );
            }

            if ( !status.isOK() ) {
                response->setErrCode( ErrorCodes::WriteConcernFailed );
                response->setErrMessage( status.toString() );
            }
            else {

                _client->curop()->setMessage( "waiting for write concern" );

                WriteConcernResult res;
                status = waitForWriteConcern( writeConcern, _client->getLastOp(), &res );
                maybeBuildWCError( status, res, response );
            }
        }

        // Set the main body of the response. We assume that, if there was an error, the error
        // code would already be set.
        response->setOk( !response->isErrCodeSet() );
        response->setN( stats.numInserted + stats.numUpserted + stats.numUpdated
                        + stats.numDeleted );
        response->setNDocsModified(stats.numModified);
        dassert( response->isValid( NULL ) );

        // TODO: Audit where we want to queue here - the shardingState calls may block for remote
        // data
        if ( staleBatch ) {

            const BatchedRequestMetadata* requestMetadata = request.getMetadata();
            dassert( requestMetadata );

            // Make sure our shard name is set or is the same as what was set previously
            if ( !shardingState.setShardName( requestMetadata->getShardName() ) ) {

                // If our shard name is stale, our version must have been stale as well
                dassert( numItemErrors == numBatchItems );
                warning() << "shard name " << requestMetadata->getShardName()
                          << " in batch does not match previously-set shard name "
                          << shardingState.getShardName() << ", not reloading metadata" << endl;
            }
            else {
                // Refresh our shard version
                ChunkVersion latestShardVersion;
                shardingState.refreshMetadataIfNeeded( request.getTargetingNS(),
                                                       requestMetadata->getShardVersion(),
                                                       &latestShardVersion );
            }
        }
    }

    namespace {

        // Translates write item type to wire protocol op code.
        // Helper for WriteBatchExecutor::applyWriteItem().
        int getOpCode( BatchedCommandRequest::BatchType writeType ) {
            switch ( writeType ) {
            case BatchedCommandRequest::BatchType_Insert:
                return dbInsert;
            case BatchedCommandRequest::BatchType_Update:
                return dbUpdate;
            default:
                dassert( writeType == BatchedCommandRequest::BatchType_Delete );
                return dbDelete;
            }
            return 0;
        }

    } // namespace

    bool WriteBatchExecutor::applyWriteItem( const BatchItemRef& itemRef,
                                             WriteStats* stats,
                                             BSONObj* upsertedID,
                                             BatchedErrorDetail* error ) {
        const BatchedCommandRequest& request = *itemRef.getRequest();
        const string& ns = request.getNS();

        // Clear operation's LastError before starting.
        _le->reset( true );

        //uint64_t itemTimeMicros = 0;
        bool opSuccess = true;

        // Each write operation executes in its own PageFaultRetryableSection.  This means that
        // a single batch can throw multiple PageFaultException's, which is not the case for
        // other operations.
        PageFaultRetryableSection s;
        while ( true ) {
            try {
                // Execute the write item as a child operation of the current operation.
                CurOp childOp( _client, _client->curop() );

                HostAndPort remote =
                    _client->hasRemote() ? _client->getRemote() : HostAndPort( "0.0.0.0", 0 );

                // TODO Modify CurOp "wrapped" constructor to take an opcode, so calling .reset()
                // is unneeded
                childOp.reset( remote, getOpCode( request.getBatchType() ) );

                childOp.ensureStarted();
                OpDebug& opDebug = childOp.debug();
                opDebug.ns = ns;
                {
                    Lock::DBWrite dbLock( ns );
                    Client::Context ctx( ns,
                                         storageGlobalParams.dbpath, // TODO: better constructor?
                                         false /* don't check version here */);

                    opSuccess = doWrite( ns, itemRef, &childOp, stats, upsertedID, error );
                }
                childOp.done();
                //itemTimeMicros = childOp.totalTimeMicros();

                opDebug.executionTime = childOp.totalTimeMillis();
                opDebug.recordStats();

                // Log operation if running with at least "-v", or if exceeds slow threshold.
                if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))
                     || opDebug.executionTime >
                        serverGlobalParams.slowMS + childOp.getExpectedLatencyMs()) {

                    MONGO_TLOG(1) << opDebug.report( childOp ) << endl;
                }

                // TODO Log operation if logLevel >= 3 and assertion thrown (as assembleResponse()
                // does).

                // Save operation to system.profile if shouldDBProfile().
                if ( childOp.shouldDBProfile( opDebug.executionTime ) ) {
                    profile( *_client, getOpCode( request.getBatchType() ), childOp );
                }
                break;
            }
            catch ( PageFaultException& e ) {
                e.touch();
            }
        }

        return opSuccess;
    }

    static void toBatchedError( const UserException& ex, BatchedErrorDetail* error ) {
        // TODO: Complex transform here?
        error->setErrCode( ex.getCode() );
        error->setErrMessage( ex.what() );
    }

    static void buildStaleError( const ChunkVersion& shardVersionRecvd,
                                 const ChunkVersion& shardVersionWanted,
                                 BatchedErrorDetail* error ) {

        // Write stale error to results
        error->setErrCode( ErrorCodes::StaleShardVersion );

        BSONObjBuilder infoB;
        shardVersionWanted.addToBSON( infoB, "vWanted" );
        error->setErrInfo( infoB.obj() );

        string errMsg = stream() << "stale shard version detected before write, received "
                                 << shardVersionRecvd.toString() << " but local version is "
                                 << shardVersionWanted.toString();
        error->setErrMessage( errMsg );
    }

    static void buildUniqueIndexError( const BSONObj& keyPattern,
                                       const BSONObj& indexPattern,
                                       BatchedErrorDetail* error ) {
        error->setErrCode( ErrorCodes::CannotCreateIndex );
        string errMsg = stream() << "cannot create unique index over " << indexPattern
                                 << " with shard key pattern " << keyPattern;
        error->setErrMessage( errMsg );
    }

    bool WriteBatchExecutor::doWrite( const string& ns,
                                      const BatchItemRef& itemRef,
                                      CurOp* currentOp,
                                      WriteStats* stats,
                                      BSONObj* upsertedID,
                                      BatchedErrorDetail* error ) {

        const BatchedCommandRequest& request = *itemRef.getRequest();
        int index = itemRef.getItemIndex();

        //
        // Check our shard version if we need to (must be in the write lock)
        //

        CollectionMetadataPtr metadata;
        if ( shardingState.enabled() ) {

            // Index inserts make the namespace nontrivial for versioning
            string targetingNS = itemRef.getRequest()->getTargetingNS();
            Lock::assertWriteLocked( targetingNS );
            metadata = shardingState.getCollectionMetadata( targetingNS );

            const BatchedRequestMetadata* requestMetadata = request.getMetadata();

            if ( requestMetadata &&
                    requestMetadata->isShardVersionSet() &&
                    !ChunkVersion::isIgnoredVersion( requestMetadata->getShardVersion() ) ) {

                ChunkVersion shardVersion =
                    metadata ? metadata->getShardVersion() : ChunkVersion::UNSHARDED();

                if ( !requestMetadata->getShardVersion() //
                    .isWriteCompatibleWith( shardVersion ) ) {

                    buildStaleError( requestMetadata->getShardVersion(), shardVersion, error );
                    return false;
                }
            }
        }

        //
        // Not stale, do the actual write
        //

        if ( request.getBatchType() == BatchedCommandRequest::BatchType_Insert ) {

            // Need to check for unique index problems
            if ( metadata && request.isUniqueIndexRequest() ) {
                if ( !isUniqueIndexCompatible( metadata->getKeyPattern(),
                                               request.getIndexKeyPattern() ) ) {

                    buildUniqueIndexError( metadata->getKeyPattern(),
                                           request.getIndexKeyPattern(),
                                           error );
                    return false;
                }
            }

            // Insert
            return doInsert( ns,
                             request.getInsertRequest()->getDocumentsAt( index ),
                             currentOp,
                             stats,
                             error );
        }
        else if ( request.getBatchType() == BatchedCommandRequest::BatchType_Update ) {

            // TODO: Pass down immutable shard key fields

            // Update
            return doUpdate( ns,
                             *request.getUpdateRequest()->getUpdatesAt( index ),
                             currentOp,
                             stats,
                             upsertedID,
                             error );
        }
        else {
            dassert( request.getBatchType() ==
                BatchedCommandRequest::BatchType_Delete );

            // Delete
            return doDelete( ns,
                             *request.getDeleteRequest()->getDeletesAt( index ),
                             currentOp,
                             stats,
                             error );
        }
    }

    bool WriteBatchExecutor::doInsert( const string& ns,
                                       const BSONObj& insertOp,
                                       CurOp* currentOp,
                                       WriteStats* stats,
                                       BatchedErrorDetail* error ) {
        OpDebug& opDebug = currentOp->debug();

        _opCounters->gotInsert();

        opDebug.op = dbInsert;

        try {
            // TODO Should call insertWithObjMod directly instead of checkAndInsert?  Note that
            // checkAndInsert will use mayInterrupt=false, so index builds initiated here won't
            // be interruptible.
            BSONObj doc = insertOp; // b/c we're const going in
            checkAndInsert( ns.c_str(), doc );
            getDur().commitIfNeeded();
            _le->nObjects = 1; // TODO Replace after implementing LastError::recordInsert().
            opDebug.ninserted = 1;
            stats->numInserted++;
        }
        catch ( const UserException& ex ) {
            opDebug.exceptionInfo = ex.getInfo();
            toBatchedError( ex, error );
            return false;
        }

        return true;
    }

    bool WriteBatchExecutor::doUpdate( const string& ns,
                                       const BatchedUpdateDocument& updateOp,
                                       CurOp* currentOp,
                                       WriteStats* stats,
                                       BSONObj* upsertedID,
                                       BatchedErrorDetail* error ) {
        OpDebug& opDebug = currentOp->debug();

        _opCounters->gotUpdate();

        BSONObj queryObj = updateOp.getQuery();
        BSONObj updateObj = updateOp.getUpdateExpr();
        bool multi = updateOp.getMulti();
        bool upsert = updateOp.getUpsert();

        currentOp->setQuery( queryObj );
        opDebug.op = dbUpdate;
        opDebug.query = queryObj;

        bool updateExisting = false;
        bool didInsert = false;
        long long numMatched = 0;
        long long numDocsModified = 0;
        BSONObj resUpsertedID;
        try {

            const NamespaceString requestNs( ns );
            UpdateRequest request( requestNs );

            request.setQuery( queryObj );
            request.setUpdates( updateObj );
            request.setUpsert( upsert );
            request.setMulti( multi );
            request.setUpdateOpLog();
            // TODO(greg) We need to send if we are ignoring the shard version below, but for now yes
            UpdateLifecycleImpl updateLifecycle(true, requestNs);
            request.setLifecycle(&updateLifecycle);

            UpdateResult res = update( request, &opDebug );

            numDocsModified = res.numDocsModified;
            updateExisting = res.existing;
            numMatched = res.numMatched;
            resUpsertedID = res.upserted;

            // We have an _id from an insert
            didInsert = !resUpsertedID.isEmpty();

            stats->numModified += didInsert ? 0 : numDocsModified;
            stats->numUpdated += didInsert ? 0 : numMatched;
            stats->numUpserted += didInsert ? 1 : 0;
        }
        catch ( const UserException& ex ) {
            opDebug.exceptionInfo = ex.getInfo();
            toBatchedError( ex, error );
            return false;
        }

        _le->recordUpdate( updateExisting, numMatched, resUpsertedID );

        if (didInsert) {
            *upsertedID = resUpsertedID;
        }

        return true;
    }

    bool WriteBatchExecutor::doDelete( const string& ns,
                                       const BatchedDeleteDocument& deleteOp,
                                       CurOp* currentOp,
                                       WriteStats* stats,
                                       BatchedErrorDetail* error ) {
        OpDebug& opDebug = currentOp->debug();

        _opCounters->gotDelete();

        BSONObj queryObj = deleteOp.getQuery();

        currentOp->setQuery( queryObj );
        opDebug.op = dbDelete;
        opDebug.query = queryObj;

        long long n;

        try {
            n = deleteObjects( ns, queryObj, // ns, query
                               deleteOp.getLimit() == 1, // justOne
                               true, // logOp
                               false // god
                               );
            stats->numDeleted += n;
        }
        catch ( const UserException& ex ) {
            opDebug.exceptionInfo = ex.getInfo();
            toBatchedError( ex, error );
            return false;
        }

        _le->recordDelete( n );
        opDebug.ndeleted = n;

        return true;
    }

} // namespace mongo
