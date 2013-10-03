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

#include "mongo/s/batch_write_exec.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h" // ConnectionString (header-only)
#include "mongo/s/batch_write_op.h"
#include "mongo/s/batched_error_detail.h"

namespace mongo {

    namespace {

        struct ConnectionStringComp {
            bool operator()( const ConnectionString* connStrA,
                             const ConnectionString* connStrB ) const {
                return connStrA->toString().compare( connStrB->toString() ) < 0;
            }
        };

        //
        // Map which allows associating ConnectionString endpoints with TargetedWriteBatches
        // This is needed since the dispatcher only returns endpoints with responses.
        //
        // TODO: Unordered map?
        typedef //
        map<const ConnectionString*, TargetedWriteBatch*, ConnectionStringComp> EndpointBatchMap;
    }

    static void buildErrorFrom( const Status& status, BatchedErrorDetail* error ) {
        error->setErrCode( status.code() );
        error->setErrMessage( status.reason() );
    }

    // Helper to note several stale errors from a response
    static void noteStaleResponses( const vector<ShardError*>& staleErrors, NSTargeter* targeter ) {
        for ( vector<ShardError*>::const_iterator it = staleErrors.begin(); it != staleErrors.end();
            ++it ) {
            const ShardError* error = *it;
            targeter->noteStaleResponse( error->endpoint, error->error.getErrInfo() );
        }
    }

    void BatchWriteExec::executeBatch( const BatchedCommandRequest& clientRequest,
                                       BatchedCommandResponse* clientResponse ) {

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &clientRequest );

        int numTargetErrors = 0;
        int numStaleBatches = 0;

        for ( int rounds = 0; !batchOp.isFinished(); rounds++ ) {

            //
            // Refresh the targeter if we need to (no-op if nothing stale)
            //

            Status refreshStatus = _targeter->refreshIfNeeded();

            if ( !refreshStatus.isOK() ) {

                // It's okay if we can't refresh, we'll just record errors for the ops if
                // needed.
                warning() << "could not refresh targeter" << causedBy( refreshStatus.reason() )
                          << endl;
            }

            //
            // Get child batches to send
            //

            vector<TargetedWriteBatch*> childBatches;

            //
            // Targeting errors can be caused by remote metadata changing (the collection could have
            // been dropped and recreated, for example with a new shard key).  If a remote metadata
            // change occurs *before* a client sends us a batch, we need to make sure that we don't
            // error out just because we're staler than the client - otherwise mongos will be have
            // unpredictable behavior.
            //
            // (If a metadata change happens *during* or *after* a client sends us a batch, however,
            // we make no guarantees about delivery.)
            //
            // For this reason, we don't record targeting errors until we've refreshed our targeting
            // metadata at least once *after* receiving the client batch - at that point, we know:
            //
            // 1) our new metadata is the same as the metadata when the client sent a batch, and so
            //    targeting errors are real.
            // OR
            // 2) our new metadata is a newer version than when the client sent a batch, and so
            //    the metadata must have changed after the client batch was sent.  We don't need to
            //    deliver in this case, since for all the client knows we may have gotten the batch
            //    exactly when the metadata changed.
            //
            // If we've had a targeting error or stale error, we've refreshed the metadata once and
            // can record target errors.
            bool recordTargetErrors = numTargetErrors > 0 || numStaleBatches > 0;

            Status targetStatus = batchOp.targetBatch( *_targeter,
                                                       recordTargetErrors,
                                                       &childBatches );
            if ( !targetStatus.isOK() ) {
                _targeter->noteCouldNotTarget();
                ++numTargetErrors;
                continue;
            }

            //
            // Send all child batches
            //

            size_t numSent = 0;
            while ( numSent != childBatches.size() ) {

                // Collect batches out on the network, mapped by endpoint
                EndpointBatchMap pendingBatches;

                //
                // Send side
                //

                // Get as many batches as we can at once
                for ( vector<TargetedWriteBatch*>::iterator it = childBatches.begin();
                    it != childBatches.end(); ++it ) {

                    TargetedWriteBatch* nextBatch = *it;
                    // If the batch is NULL, we sent it previously, so skip
                    if ( nextBatch == NULL ) continue;
                    const ConnectionString& hostEndpoint = nextBatch->getEndpoint().shardHost;

                    EndpointBatchMap::iterator pendingIt = pendingBatches.find( &hostEndpoint );

                    // If we already have a batch for this endpoint, continue
                    if ( pendingIt != pendingBatches.end() ) continue;

                    // Otherwise send it out to the endpoint

                    BatchedCommandRequest request( clientRequest.getBatchType() );
                    batchOp.buildBatchRequest( *nextBatch, &request );
                    _dispatcher->addCommand( hostEndpoint, request );

                    // Indicate we're done by setting the batch to NULL
                    // We'll only get duplicate hostEndpoints if we have broadcast and non-broadcast
                    // endpoints for the same host, so this should be pretty efficient without
                    // moving stuff around.
                    *it = NULL;

                    // Recv-side is responsible for cleaning up the nextBatch when used
                    pendingBatches.insert( make_pair( &hostEndpoint, nextBatch ) );
                }

                // Send them all out
                _dispatcher->sendAll();
                numSent += pendingBatches.size();

                //
                // Recv side
                //

                while ( _dispatcher->numPending() > 0 ) {

                    // Get the response
                    ConnectionString endpoint;
                    BatchedCommandResponse response;
                    Status dispatchStatus = _dispatcher->recvAny( &endpoint, &response );

                    // Get the TargetedWriteBatch to find where to put the response
                    TargetedWriteBatch* batchRaw = pendingBatches.find( &endpoint )->second;
                    scoped_ptr<TargetedWriteBatch> batch( batchRaw );

                    if ( dispatchStatus.isOK() ) {

                        TrackedErrors trackedErrors;
                        trackedErrors.startTracking( ErrorCodes::StaleShardVersion );

                        // Dispatch was ok, note response
                        batchOp.noteBatchResponse( *batch, response, &trackedErrors );

                        // Note if anything was stale
                        const vector<ShardError*>& staleErrors =
                            trackedErrors.getErrors( ErrorCodes::StaleShardVersion );

                        if ( staleErrors.size() > 0 ) {
                            noteStaleResponses( staleErrors, _targeter );
                            ++numStaleBatches;
                        }
                    }
                    else {

                        // Error occurred dispatching, note it
                        BatchedErrorDetail error;
                        buildErrorFrom( dispatchStatus, &error );
                        batchOp.noteBatchError( *batch, error );
                    }
                }
            }
        }

        batchOp.buildClientResponse( clientResponse );
    }

}
