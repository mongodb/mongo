/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/write_ops/batch_write_op.h"

#include "mongo/base/error_codes.h"

namespace mongo {

    BatchWriteStats::BatchWriteStats() :
        numInserted( 0 ), numUpserted( 0 ), numUpdated( 0 ), numModified( 0 ), numDeleted( 0 ) {
    }

    BatchWriteOp::BatchWriteOp() :
        _clientRequest( NULL ), _writeOps( NULL ), _stats( new BatchWriteStats ) {
    }

    void BatchWriteOp::initClientRequest( const BatchedCommandRequest* clientRequest ) {
        dassert( clientRequest->isValid( NULL ) );

        size_t numWriteOps = clientRequest->sizeWriteOps();
        _writeOps = static_cast<WriteOp*>( ::operator new[]( numWriteOps * sizeof(WriteOp) ) );

        for ( size_t i = 0; i < numWriteOps; ++i ) {
            // Don't want to have to define what an empty WriteOp means, so construct in-place
            new ( &_writeOps[i] ) WriteOp( BatchItemRef( clientRequest, i ) );
        }

        _clientRequest = clientRequest;
    }

    static void buildTargetError( const Status& errStatus, WriteErrorDetail* details ) {
        details->setErrCode( errStatus.code() );
        details->setErrMessage( errStatus.reason() );
    }

    // Arbitrary endpoint ordering, needed for grouping by endpoint
    static int compareEndpoints( const ShardEndpoint* endpointA, const ShardEndpoint* endpointB ) {

        int shardNameDiff = endpointA->shardName.compare( endpointB->shardName );
        if ( shardNameDiff != 0 ) return shardNameDiff;

        long shardVersionDiff = endpointA->shardVersion.toLong() - endpointB->shardVersion.toLong();
        if ( shardVersionDiff != 0 ) return shardVersionDiff;

        int shardEpochDiff =
            endpointA->shardVersion.epoch().compare( endpointB->shardVersion.epoch() );
        return shardEpochDiff;
    }

    namespace {

        //
        // Types for comparing shard endpoints in a map
        //

        struct EndpointComp {
            bool operator()( const ShardEndpoint* endpointA,
                             const ShardEndpoint* endpointB ) const {
                return compareEndpoints( endpointA, endpointB ) < 0;
            }
        };

        typedef std::map<const ShardEndpoint*, TargetedWriteBatch*, EndpointComp> TargetedBatchMap;
    }

    // Helper function to cancel all the write ops of targeted batch.
    static void cancelBatch( const TargetedWriteBatch& targetedBatch,
                             WriteOp* writeOps,
                             const WriteErrorDetail& why ) {
        const vector<TargetedWrite*>& writes = targetedBatch.getWrites();

        for ( vector<TargetedWrite*>::const_iterator writeIt = writes.begin();
            writeIt != writes.end(); ++writeIt ) {

            TargetedWrite* write = *writeIt;
            // NOTE: We may repeatedly cancel a write op here, but that's fast.
            writeOps[write->writeOpRef.first].cancelWrites( &why );
        }
    }

    // Helper function to cancel all the write ops of targeted batches in a map
    static void cancelBatches( const WriteErrorDetail& why,
                               WriteOp* writeOps,
                               TargetedBatchMap* batchMap ) {

        set<WriteOp*> targetedWriteOps;

        // Collect all the writeOps that are currently targeted
        for ( TargetedBatchMap::iterator it = batchMap->begin(); it != batchMap->end(); ) {

            TargetedWriteBatch* batch = it->second;
            const vector<TargetedWrite*>& writes = batch->getWrites();

            for ( vector<TargetedWrite*>::const_iterator writeIt = writes.begin();
                writeIt != writes.end(); ++writeIt ) {

                TargetedWrite* write = *writeIt;

                // NOTE: We may repeatedly cancel a write op here, but that's fast and we want to
                // cancel before erasing the TargetedWrite* (which owns the cancelled targeting
                // info) for reporting reasons.
                writeOps[write->writeOpRef.first].cancelWrites( &why );
            }

            // Note that we need to *erase* first, *then* delete, since the map keys are ptrs from
            // the values
            batchMap->erase( it++ );
            delete batch;
        }
        batchMap->clear();
    }

    Status BatchWriteOp::targetBatch( const NSTargeter& targeter,
                                      bool recordTargetErrors,
                                      vector<TargetedWriteBatch*>* targetedBatches ) {

        TargetedBatchMap batchMap;
        int numTargetErrors = 0;

        size_t numWriteOps = _clientRequest->sizeWriteOps();
        for ( size_t i = 0; i < numWriteOps; ++i ) {

            // Only do one-at-a-time ops if COE is false (and break at first target error)
            if ( _clientRequest->getOrdered() && ( !batchMap.empty() || numTargetErrors != 0 ) )
                break;

            WriteOp& writeOp = _writeOps[i];

            // Only target _Ready ops
            if ( writeOp.getWriteState() != WriteOpState_Ready ) continue;

            //
            // Get TargetedWrites from the targeter for the write operation
            //

            // TargetedWrites need to be owned once returned
            OwnedPointerVector<TargetedWrite> writesOwned;
            vector<TargetedWrite*>& writes = writesOwned.mutableVector();

            Status targetStatus = writeOp.targetWrites( targeter, &writes );

            if ( !targetStatus.isOK() ) {

                //
                // We're not sure how to target here, so either record the error or cancel the
                // current batches.
                //

                WriteErrorDetail targetError;
                buildTargetError( targetStatus, &targetError );

                if ( recordTargetErrors ) {
                    writeOp.setOpError( targetError );
                    ++numTargetErrors;
                    continue;
                }
                else {
                    // Cancel current batch state with an error
                    cancelBatches( targetError, _writeOps, &batchMap );
                    dassert( batchMap.empty() );
                    return targetStatus;
                }
            }

            //
            // Targeting went ok, add to appropriate TargetedBatch
            //

            for ( vector<TargetedWrite*>::iterator it = writes.begin(); it != writes.end(); ++it ) {

                TargetedWrite* write = *it;

                TargetedBatchMap::iterator seenIt = batchMap.find( &write->endpoint );
                if ( seenIt == batchMap.end() ) {
                    TargetedWriteBatch* newBatch = new TargetedWriteBatch( write->endpoint );
                    seenIt = batchMap.insert( make_pair( &newBatch->getEndpoint(), //
                                                         newBatch ) ).first;
                }

                TargetedWriteBatch* batch = seenIt->second;
                batch->addWrite( write );
            }

            // Relinquish ownership of TargetedWrites, now the TargetedBatches own them
            writesOwned.mutableVector().clear();
        }

        //
        // Send back our targeted batches
        //

        for ( TargetedBatchMap::iterator it = batchMap.begin(); it != batchMap.end(); ++it ) {

            TargetedWriteBatch* batch = it->second;

            // Remember targeted batch for reporting
            _targeted.insert( batch );
            // Send the handle back to caller
            targetedBatches->push_back( batch );
        }

        return Status::OK();
    }

    void BatchWriteOp::buildBatchRequest( const TargetedWriteBatch& targetedBatch,
                                          BatchedCommandRequest* request ) const {

        request->setNS( _clientRequest->getNS() );

        const vector<TargetedWrite*>& targetedWrites = targetedBatch.getWrites();

        for ( vector<TargetedWrite*>::const_iterator it = targetedWrites.begin();
            it != targetedWrites.end(); ++it ) {

            const WriteOpRef& writeOpRef = ( *it )->writeOpRef;
            BatchedCommandRequest::BatchType batchType = _clientRequest->getBatchType();

            // NOTE:  We copy the batch items themselves here from the client request
            // TODO: This could be inefficient, maybe we want to just reference in the future
            if ( batchType == BatchedCommandRequest::BatchType_Insert ) {
                BatchedInsertRequest* clientInsertRequest = _clientRequest->getInsertRequest();
                BSONObj insertDoc = clientInsertRequest->getDocumentsAt( writeOpRef.first );
                request->getInsertRequest()->addToDocuments( insertDoc );
            }
            else if ( batchType == BatchedCommandRequest::BatchType_Update ) {
                BatchedUpdateRequest* clientUpdateRequest = _clientRequest->getUpdateRequest();
                BatchedUpdateDocument* updateDoc = new BatchedUpdateDocument;
                clientUpdateRequest->getUpdatesAt( writeOpRef.first )->cloneTo( updateDoc );
                request->getUpdateRequest()->addToUpdates( updateDoc );
            }
            else {
                dassert( batchType == BatchedCommandRequest::BatchType_Delete );
                BatchedDeleteRequest* clientDeleteRequest = _clientRequest->getDeleteRequest();
                BatchedDeleteDocument* deleteDoc = new BatchedDeleteDocument;
                clientDeleteRequest->getDeletesAt( writeOpRef.first )->cloneTo( deleteDoc );
                request->getDeleteRequest()->addToDeletes( deleteDoc );
            }

            // TODO: We can add logic here to allow aborting individual ops
            //if ( NULL == response ) {
            //    ->responses.erase( it++ );
            //    continue;
            //}
        }

        if ( _clientRequest->isWriteConcernSet() ) {
            request->setWriteConcern( _clientRequest->getWriteConcern() );
        }

        if ( !request->isOrderedSet() ) {
            request->setOrdered( _clientRequest->getOrdered() );
        }

        auto_ptr<BatchedRequestMetadata> requestMetadata( new BatchedRequestMetadata() );
        requestMetadata->setShardName( targetedBatch.getEndpoint().shardName );
        requestMetadata->setShardVersion( targetedBatch.getEndpoint().shardVersion );
        requestMetadata->setSession( 0 );
        request->setMetadata( requestMetadata.release() );
    }

    //
    // Helpers for manipulating batch responses
    //

    namespace {
        struct WriteErrorDetailComp {
            bool operator()( const WriteErrorDetail* errorA,
                             const WriteErrorDetail* errorB ) const {
                return errorA->getIndex() < errorB->getIndex();
            }
        };
    }

    static void cloneBatchErrorTo( const BatchedCommandResponse& batchResp,
                                   WriteErrorDetail* details ) {
        details->setErrCode( batchResp.getErrCode() );
        if ( batchResp.isErrInfoSet() ) details->setErrInfo( batchResp.getErrInfo() );
        details->setErrMessage( batchResp.getErrMessage() );
    }

    static void cloneBatchErrorFrom( const WriteErrorDetail& details,
                                     BatchedCommandResponse* response ) {
        response->setErrCode( details.getErrCode() );
        if ( details.isErrInfoSet() ) response->setErrInfo( details.getErrInfo() );
        response->setErrMessage( details.getErrMessage() );
    }

    // Given *either* a batch error or an array of per-item errors, copies errors we're interested
    // in into a TrackedErrorMap
    static void trackErrors( const ShardEndpoint& endpoint,
                             const vector<WriteErrorDetail*> itemErrors,
                             TrackedErrors* trackedErrors ) {
        for ( vector<WriteErrorDetail*>::const_iterator it = itemErrors.begin();
            it != itemErrors.end(); ++it ) {

            const WriteErrorDetail* error = *it;

            if ( trackedErrors->isTracking( error->getErrCode() ) ) {
                trackedErrors->addError( new ShardError( endpoint, *error ) );
            }
        }
    }

    static void incBatchStats( BatchedCommandRequest::BatchType batchType,
                               const BatchedCommandResponse& response,
                               BatchWriteStats* stats ) {

        if ( batchType == BatchedCommandRequest::BatchType_Insert) {
            stats->numInserted += response.getN();
        }
        else if ( batchType == BatchedCommandRequest::BatchType_Update ) {
            int numUpserted = 0;
            if( response.isUpsertDetailsSet() ) {
                numUpserted = response.sizeUpsertDetails();
            }
            else if( response.isSingleUpsertedSet() ) {
                numUpserted = 1;
            }
            stats->numUpdated += ( response.getN() - numUpserted );
            stats->numModified += response.getNDocsModified();
            stats->numUpserted += numUpserted;
        }
        else {
            dassert( batchType == BatchedCommandRequest::BatchType_Delete );
            stats->numDeleted += response.getN();
        }
    }

    void BatchWriteOp::noteBatchResponse( const TargetedWriteBatch& targetedBatch,
                                          const BatchedCommandResponse& response,
                                          TrackedErrors* trackedErrors ) {

        // Increment stats for this batch
        incBatchStats( _clientRequest->getBatchType(), response, _stats.get() );

        // Stop tracking targeted batch
        _targeted.erase( &targetedBatch );

        //
        // Organize errors based on error code.
        // We may have *either* a batch error or errors per-item.
        // (Write Concern errors are stored and handled later.)
        //

        if ( !response.getOk() ) {
            WriteErrorDetail batchError;
            cloneBatchErrorTo( response, &batchError );
            _batchCommandError.reset( new ShardError( targetedBatch.getEndpoint(),
                                                      batchError ));
            cancelBatch( targetedBatch, _writeOps, batchError );
            return;
        }

        // Special handling for write concern errors, save for later
        if ( response.isWriteConcernErrorSet() ) {
            // TODO: XXX use WCErrorDetail?
            WriteErrorDetail error;
            cloneBatchErrorTo( response, &error );
            ShardError* wcError = new ShardError( targetedBatch.getEndpoint(), error );
            _wcErrors.mutableVector().push_back( wcError );
        }

        vector<WriteErrorDetail*> itemErrors;

        // Handle batch and per-item errors
        if ( response.isErrDetailsSet() ) {

            // Per-item errors were set
            itemErrors.insert( itemErrors.begin(),
                               response.getErrDetails().begin(),
                               response.getErrDetails().end() );

            // Sort per-item errors by index
            std::sort( itemErrors.begin(), itemErrors.end(), WriteErrorDetailComp() );
        }

        //
        // Go through all pending responses of the op and sorted remote reponses, populate errors
        // This will either set all errors to the batch error or apply per-item errors as-needed
        //

        vector<WriteErrorDetail*>::iterator itemErrorIt = itemErrors.begin();
        int index = 0;
        for ( vector<TargetedWrite*>::const_iterator it = targetedBatch.getWrites().begin();
            it != targetedBatch.getWrites().end(); ++it, ++index ) {

            const TargetedWrite* write = *it;
            WriteOp& writeOp = _writeOps[write->writeOpRef.first];

            dassert( writeOp.getWriteState() == WriteOpState_Pending );

            // See if we have an error for the write
            WriteErrorDetail* writeError = NULL;

            if ( itemErrorIt != itemErrors.end() && ( *itemErrorIt )->getIndex() == index ) {
                // We have an per-item error for this write op's index
                writeError = *itemErrorIt;
                ++itemErrorIt;
            }

            // Finish the response (with error, if needed)
            if ( NULL == writeError ) {
                writeOp.noteWriteComplete( *write );
            }
            else {
                writeOp.noteWriteError( *write, *writeError );
            }
        }

        // Track errors we care about, whether batch or individual errors
        if ( NULL != trackedErrors ) {
            trackErrors( targetedBatch.getEndpoint(), itemErrors, trackedErrors );
        }

        // Track upserted ids if we need to
        if ( response.isSingleUpsertedSet() ) {

            // Work backward from the child batch item index to the batch item index
            int batchIndex = targetedBatch.getWrites()[0]->writeOpRef.first;

            BatchedUpsertDetail* upsertedId = new BatchedUpsertDetail;
            upsertedId->setIndex( batchIndex );
            upsertedId->setUpsertedID( response.getSingleUpserted() );
            _upsertedIds.mutableVector().push_back( upsertedId );
        }
        else if ( response.isUpsertDetailsSet() ) {

            const vector<BatchedUpsertDetail*>& upsertedIds = response.getUpsertDetails();
            for ( vector<BatchedUpsertDetail*>::const_iterator it = upsertedIds.begin();
                it != upsertedIds.end(); ++it ) {

                // The child upserted details don't have the correct index for the full batch
                const BatchedUpsertDetail* childUpsertedId = *it;

                // Work backward from the child batch item index to the batch item index
                int childBatchIndex = childUpsertedId->getIndex();
                int batchIndex = targetedBatch.getWrites()[childBatchIndex]->writeOpRef.first;

                // Push the upserted id with the correct index into the batch upserted ids
                BatchedUpsertDetail* upsertedId = new BatchedUpsertDetail;
                upsertedId->setIndex( batchIndex );
                upsertedId->setUpsertedID( childUpsertedId->getUpsertedID() );
                _upsertedIds.mutableVector().push_back( upsertedId );
            }
        }
    }

    void BatchWriteOp::noteBatchError( const TargetedWriteBatch& targetedBatch,
                                       const WriteErrorDetail& error ) {
        BatchedCommandResponse response;
        response.setOk( false );
        response.setN( 0 );
        cloneBatchErrorFrom( error, &response );
        dassert( response.isValid( NULL ) );
        noteBatchResponse( targetedBatch, response, NULL );
    }

    bool BatchWriteOp::isFinished() {
        if ( _batchCommandError.get() ) return true;

        size_t numWriteOps = _clientRequest->sizeWriteOps();
        bool orderedOps = _clientRequest->getOrdered();
        for ( size_t i = 0; i < numWriteOps; ++i ) {
            WriteOp& writeOp = _writeOps[i];
            if ( writeOp.getWriteState() < WriteOpState_Completed ) return false;
            else if ( orderedOps && writeOp.getWriteState() == WriteOpState_Error ) return true;
        }

        return true;
    }

    //
    // Aggregation functions for building the final response errors
    //

    void BatchWriteOp::buildClientResponse( BatchedCommandResponse* batchResp ) {

        dassert( isFinished() );

        if ( _batchCommandError.get() ) {
            batchResp->setOk( 0 );

            const WriteErrorDetail& error = _batchCommandError->error;
            batchResp->setErrCode( error.getErrCode() );

            string errMsg( str::stream() << error.getErrMessage() << " at "
                                         << _batchCommandError->endpoint.shardName );
            batchResp->setErrMessage( errMsg );
            return;
        }

        //
        // Find all the errors in the batch
        //

        vector<WriteOp*> errOps;

        size_t numWriteOps = _clientRequest->sizeWriteOps();
        for ( size_t i = 0; i < numWriteOps; ++i ) {

            WriteOp& writeOp = _writeOps[i];

            if ( writeOp.getWriteState() == WriteOpState_Error ) {
                errOps.push_back( &writeOp );
            }
        }

        //
        // Build the per-item errors.
        //

        if ( !errOps.empty() && _clientRequest->isVerboseWC() ) {
            for ( vector<WriteOp*>::iterator it = errOps.begin(); it != errOps.end(); ++it ) {
                WriteOp& writeOp = **it;
                WriteErrorDetail* error = new WriteErrorDetail();
                writeOp.getOpError().cloneTo( error );
                batchResp->addToErrDetails( error );
            }
        }

        //
        // Append the upserted ids, if required
        //

        if ( _upsertedIds.size() != 0 ) {
            if ( _clientRequest->sizeWriteOps() == 1u ) {
                batchResp->setSingleUpserted( _upsertedIds.vector().front()->getUpsertedID() );
            }
            else if( _clientRequest->isVerboseWC() ) {
                batchResp->setUpsertDetails( _upsertedIds.vector() );
            }
        }

        // Stats
        int nValue = _stats->numInserted + _stats->numUpserted + _stats->numUpdated
                     + _stats->numDeleted;
        batchResp->setN( nValue );
        if ( _clientRequest->getBatchType() == BatchedCommandRequest::BatchType_Update )
            batchResp->setNDocsModified( _stats->numModified );

        batchResp->setOk( !batchResp->isErrCodeSet() );
        dassert( batchResp->isValid( NULL ) );
    }

    BatchWriteOp::~BatchWriteOp() {
        // Caller's responsibility to dispose of TargetedBatches
        dassert( _targeted.empty() );

        if ( NULL != _writeOps ) {

            size_t numWriteOps = _clientRequest->sizeWriteOps();
            for ( size_t i = 0; i < numWriteOps; ++i ) {
                // Placement new so manual destruct
                _writeOps[i].~WriteOp();
            }

            ::operator delete[]( _writeOps );
            _writeOps = NULL;
        }
    }

    void TrackedErrors::startTracking( int errCode ) {
        dassert( !isTracking( errCode ) );
        _errorMap.insert( make_pair( errCode, vector<ShardError*>() ) );
    }

    bool TrackedErrors::isTracking( int errCode ) const {
        return _errorMap.find( errCode ) != _errorMap.end();
    }

    void TrackedErrors::addError( ShardError* error ) {
        TrackedErrorMap::iterator seenIt = _errorMap.find( error->error.getErrCode() );
        if ( seenIt == _errorMap.end() ) return;
        seenIt->second.push_back( error );
    }

    const vector<ShardError*>& TrackedErrors::getErrors( int errCode ) const {
        dassert( isTracking( errCode ) );
        return _errorMap.find( errCode )->second;
    }

    void TrackedErrors::clear() {
        for ( TrackedErrorMap::iterator it = _errorMap.begin(); it != _errorMap.end(); ++it ) {

            vector<ShardError*>& errors = it->second;

            for ( vector<ShardError*>::iterator errIt = errors.begin(); errIt != errors.end();
                ++errIt ) {
                delete *errIt;
            }
            errors.clear();
        }
    }

    TrackedErrors::~TrackedErrors() {
        clear();
    }

}
