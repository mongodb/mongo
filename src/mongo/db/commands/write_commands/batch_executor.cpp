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
#include "mongo/db/pagefault.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/batched_error_detail.h"
#include "mongo/s/batched_upsert_detail.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/d_logic.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace {

        // We're assuming here that writeConcern was parsed and is valid or is not
        // present. This is just a quick check of whether is is {w:0} or not.
        bool verboseResponse( const BatchedCommandRequest& request ) {
            if ( !request.isWriteConcernSet() ) {
                return true;
            }

            BSONObj writeConcern = request.getWriteConcern();
            BSONElement wElem = writeConcern["w"];
            if ( !wElem.isNumber() || wElem.Number() != 0 ) {
                return true;
            }

            return false;
        }

    }

    WriteBatchExecutor::WriteBatchExecutor( const BSONObj& wc,
                                            Client* client,
                                            OpCounters* opCounters,
                                            LastError* le )
        : _defaultWriteConcern(wc), _client( client ), _opCounters( opCounters ), _le( le ) {
    }

    void WriteBatchExecutor::executeBatch( const BatchedCommandRequest& request,
                                           BatchedCommandResponse* response ) {

        Timer commandTimer;

        WriteStats stats;
        std::auto_ptr<BatchedErrorDetail> error( new BatchedErrorDetail );
        BSONObj upsertedID = BSONObj();
        bool batchSuccess = true;
        bool staleBatch = false;

        // Apply each batch item, stopping on an error if we were asked to apply the batch
        // sequentially.
        size_t numBatchOps = request.sizeWriteOps();
        bool verbose = verboseResponse( request );
        for ( size_t i = 0; i < numBatchOps; i++ ) {

            if ( applyWriteItem( BatchItemRef( &request, i ),
                                 &stats,
                                 &upsertedID,
                                 error.get() ) ) {

                // In case updates turned out to be upserts, the callers may be interested
                // in learning what _id was used for that document.
                if (!upsertedID.isEmpty()) {
                    if (numBatchOps == 1) {
                        response->setSingleUpserted(upsertedID);
                    }
                    else {
                        std::auto_ptr<BatchedUpsertDetail> upsertDetail(new BatchedUpsertDetail);
                        upsertDetail->setIndex(i);
                        upsertDetail->setUpsertedID(upsertedID);
                        response->addToUpsertDetails(upsertDetail.release());
                    }
                    upsertedID = BSONObj();
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
                if (verbose || numBatchOps == 1) {
                    error->setIndex( static_cast<int>( i ) );
                    response->addToErrDetails( error.release() );
                }

                batchSuccess = false;

                if ( request.getOrdered() ) break;

                error.reset( new BatchedErrorDetail );
            }
        }

        // So far, we may have failed some of the batch's items. So we record
        // that. Rergardless, we still need to apply the write concern.  If that generates a
        // more specific error, we'd replace for the intermediate error here. Note that we
        // "compatct" the error messge if this is an one-item batch. (See rationale later in
        // this file.)
        if ( !batchSuccess ) {

            if (numBatchOps > 1) {
                // TODO
                // Define the final error code here.
                // Might be used as a final error, depending on write concern success.
                response->setErrCode( 99999 );
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

        // Apply write concern. Note, again, that we're only assembling a full response if the
        // user is interested in it.
        BSONObj writeConcern;
        if ( request.isWriteConcernSet() ) {
            writeConcern = request.getWriteConcern();
        }
        else {
            writeConcern = _defaultWriteConcern;
        }

        string errMsg;
        BSONObjBuilder wcResultsB;
        if ( !waitForWriteConcern( writeConcern, !batchSuccess, &wcResultsB, &errMsg ) ) {

            // TODO Revisit when user visiible family error codes are set
            response->setErrCode( ErrorCodes::WriteConcernFailed );
            response->setErrMessage( errMsg );

            if ( verbose ) {
                response->setErrInfo( wcResultsB.obj() );
            }
        }

        // TODO: Audit where we want to queue here
        if ( staleBatch ) {
            ChunkVersion latestShardVersion;
            shardingState.refreshMetadataIfNeeded( request.getNS(),
                                                   request.getShardVersion(),
                                                   &latestShardVersion );
        }

        // Set the main body of the response. We assume that, if there was an error, the error
        // code would already be set.
        response->setOk( !response->isErrCodeSet() );
        response->setN( stats.numUpdated + stats.numInserted + stats.numDeleted );
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

        if ( shardingState.enabled() && request.isShardVersionSet()
             && !ChunkVersion::isIgnoredVersion( request.getShardVersion() ) ) {

            Lock::assertWriteLocked( ns );
            CollectionMetadataPtr metadata = shardingState.getCollectionMetadata( ns );
            ChunkVersion shardVersion =
                metadata ? metadata->getShardVersion() : ChunkVersion::UNSHARDED();

            if ( !request.getShardVersion() //
                .isWriteCompatibleWith( shardVersion ) ) {

                // Write stale error to results
                error->setErrCode( ErrorCodes::StaleShardVersion );

                BSONObjBuilder infoB;
                shardVersion.addToBSON( infoB, "vWanted" );
                error->setErrInfo( infoB.obj() );

                string errMsg = mongoutils::str::stream()
                    << "stale shard version detected before write, received "
                    << request.getShardVersion().toString() << " but local version is "
                    << shardVersion.toString();
                error->setErrMessage( errMsg );

                return false;
            }
        }

        //
        // Not stale, do the actual write
        //

        if ( request.getBatchType() == BatchedCommandRequest::BatchType_Insert ) {

            // Insert
            return doInsert( ns,
                             request.getInsertRequest()->getDocumentsAt( index ),
                             currentOp,
                             stats,
                             error );
        }
        else if ( request.getBatchType() == BatchedCommandRequest::BatchType_Update ) {

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
        bool multi = updateOp.isMultiSet() ? updateOp.getMulti() : false;
        bool upsert = updateOp.isUpsertSet() ? updateOp.getUpsert() : false;

        currentOp->setQuery( queryObj );
        opDebug.op = dbUpdate;
        opDebug.query = queryObj;

        bool updateExisting = false;
        long long numUpdated = 0;
        BSONObj resUpsertedID;
        try {

            const NamespaceString requestNs( ns );
            UpdateRequest request( requestNs );

            request.setQuery( queryObj );
            request.setUpdates( updateObj );
            request.setUpsert( upsert );
            request.setMulti( multi );
            request.setUpdateOpLog();

            UpdateResult res = update( request, &opDebug );

            updateExisting = res.existing;
            numUpdated = res.numMatched;
            resUpsertedID = res.upserted;

            stats->numUpdated += resUpsertedID.isEmpty() ? numUpdated : 0;
            stats->numUpserted += !resUpsertedID.isEmpty() ? 1 : 0;
        }
        catch ( const UserException& ex ) {
            opDebug.exceptionInfo = ex.getInfo();
            toBatchedError( ex, error );
            return false;
        }

        _le->recordUpdate( updateExisting, numUpdated, resUpsertedID );

        if (!resUpsertedID.isEmpty()) {
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
                               false, // justOne
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
