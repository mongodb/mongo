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

#pragma once

#include <set>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/wc_error_detail.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/s/write_ops/write_op.h"

namespace mongo {

    class TargetedWriteBatch;
    struct ShardError;
    struct ShardWCError;
    class TrackedErrors;
    struct BatchWriteStats;

    /**
     * The BatchWriteOp class manages the lifecycle of a batched write received by mongos.  Each
     * item in a batch is tracked via a WriteOp, and the function of the BatchWriteOp is to
     * aggregate the dispatched requests and responses for the underlying WriteOps.
     *
     * Overall, the BatchWriteOp lifecycle is similar to the WriteOp lifecycle, with the following
     * stages:
     *
     * 0) Client request comes in, batch write op is initialized
     *
     * 1a) One or more ops in the batch are targeted using targetBatch, resulting in
     *     TargetedWriteBatches for these ops.
     * 1b) There are targeting errors, and the batch must be retargeted after refreshing the
     *     NSTargeter.
     *
     * 2) (Child BatchCommandRequests are be built for each TargetedWriteBatch before sending)
     *
     * 3) Responses for sent TargetedWriteBatches are noted, errors are stored and aggregated per-
     *    write-op.  Errors the caller is interested in are returned.
     *
     * 4) If the batch write is not finished, goto 0
     *
     * 5) When all responses come back for all write ops, errors are aggregated and returned in
     *    a client response
     *
     */
    class BatchWriteOp {
    MONGO_DISALLOW_COPYING(BatchWriteOp);
    public:

        BatchWriteOp();

        ~BatchWriteOp();

        /**
         * Initializes the BatchWriteOp from a client batch request.
         */
        void initClientRequest( const BatchedCommandRequest* clientRequest );

        /**
         * Targets one or more of the next write ops in this batch op using a NSTargeter.  The
         * resulting TargetedWrites are aggregated together in the returned TargetedWriteBatches.
         *
         * If 'recordTargetErrors' is false, any targeting error will abort all current batches and
         * the method will return the targeting error.  No targetedBatches will be returned on
         * error.
         *
         * Otherwise, if 'recordTargetErrors' is true, targeting errors will be recorded for each
         * write op that fails to target, and the method will return OK.
         *
         * (The idea here is that if we are sure our NSTargeter is up-to-date we should record
         * targeting errors, but if not we should refresh once first.)
         *
         * Returned TargetedWriteBatches are owned by the caller.
         */
        Status targetBatch( const NSTargeter& targeter,
                            bool recordTargetErrors,
                            vector<TargetedWriteBatch*>* targetedBatches );

        /**
         * Fills a BatchCommandRequest from a TargetedWriteBatch for this BatchWriteOp.
         */
        void buildBatchRequest( const TargetedWriteBatch& targetedBatch,
                                BatchedCommandRequest* request ) const;

        /**
         * Stores a response from one of the outstanding TargetedWriteBatches for this BatchWriteOp.
         * The response may be in any form, error or not.
         *
         * There is an additional optional 'trackedErrors' parameter, which can be used to return
         * copies of any errors in the response that the caller is interested in (specified by
         * errCode).  (This avoids external callers needing to know much about the response format.)
         */
        void noteBatchResponse( const TargetedWriteBatch& targetedBatch,
                                const BatchedCommandResponse& response,
                                TrackedErrors* trackedErrors );

        /**
         * Stores an error that occurred while trying to send/recv a TargetedWriteBatch for this
         * BatchWriteOp, and so a response is not available.
         */
        void noteBatchError( const TargetedWriteBatch& targetedBatch,
                             const WriteErrorDetail& error );

        /**
         * Sets a command error for this batch op directly.
         *
         * Should only be used when there are no outstanding batches to return.
         */
        void setBatchError( const WriteErrorDetail& error );

        /**
         * Returns false if the batch write op needs more processing.
         */
        bool isFinished();

        /**
         * Fills a batch response to send back to the client.
         */
        void buildClientResponse( BatchedCommandResponse* batchResp );

        //
        // Accessors
        //

        int numWriteOps() const;

        int numWriteOpsIn( WriteOpState state ) const;

    private:

        // Incoming client request, not owned here
        const BatchedCommandRequest* _clientRequest;

        // Array of ops being processed from the client request
        WriteOp* _writeOps;

        // Current outstanding batch op write requests
        // Not owned here but tracked for reporting
        std::set<const TargetedWriteBatch*> _targeted;

        // Write concern responses from all write batches so far
        OwnedPointerVector<ShardWCError> _wcErrors;

        // Upserted ids for the whole write batch
        OwnedPointerVector<BatchedUpsertDetail> _upsertedIds;

        // Use to store a top-level error indicating that the batch aborted unexpectedly and we
        // can't report on any of the writes sent.  May also include a ShardEndpoint indicating
        // where the root problem was.
        scoped_ptr<ShardError> _batchError;

        // Stats for the entire batch op
        scoped_ptr<BatchWriteStats> _stats;
    };

    struct BatchWriteStats {

        BatchWriteStats();

        int numInserted;
        int numUpserted;
        int numUpdated;
        int numModified;
        int numDeleted;

    };

    /**
     * Data structure representing the information needed to make a batch request, along with
     * pointers to where the resulting responses should be placed.
     *
     * Internal support for storage as a doubly-linked list, to allow the TargetedWriteBatch to
     * efficiently be registered for reporting.
     */
    class TargetedWriteBatch {
    MONGO_DISALLOW_COPYING(TargetedWriteBatch);
    public:

        TargetedWriteBatch( const ShardEndpoint& endpoint ) :
            _endpoint( endpoint ) {
        }

        const ShardEndpoint& getEndpoint() const {
            return _endpoint;
        }

        /**
         * TargetedWrite is owned here once given to the TargetedWriteBatch
         */
        void addWrite( TargetedWrite* targetedWrite ) {
            _writes.mutableVector().push_back( targetedWrite );
        }

        const std::vector<TargetedWrite*>& getWrites() const {
            return _writes.vector();
        }

    private:

        // Where to send the batch
        const ShardEndpoint _endpoint;

        // Where the responses go
        // TargetedWrite*s are owned by the TargetedWriteBatch
        OwnedPointerVector<TargetedWrite> _writes;
    };

    /**
     * Simple struct for storing an error with an endpoint.
     *
     * Certain types of errors are not stored in WriteOps or must be returned to a caller.
     */
    struct ShardError {

        ShardError( const ShardEndpoint& endpoint, const WriteErrorDetail& error ) :
            endpoint( endpoint ) {
            error.cloneTo( &this->error );
        }

        const ShardEndpoint endpoint;
        WriteErrorDetail error;
    };

    /**
     * Simple struct for storing a write concern error with an endpoint.
     *
     * Certain types of errors are not stored in WriteOps or must be returned to a caller.
     */
    struct ShardWCError {

        ShardWCError( const ShardEndpoint& endpoint, const WCErrorDetail& error ) :
            endpoint( endpoint ) {
            error.cloneTo( &this->error );
        }

        const ShardEndpoint endpoint;
        WCErrorDetail error;
    };

    /**
     * Helper class for tracking certain errors from batch operations
     */
    class TrackedErrors {
    public:

        ~TrackedErrors();

        void startTracking( int errCode );

        bool isTracking( int errCode ) const;

        void addError( ShardError* error );

        const std::vector<ShardError*>& getErrors( int errCode ) const;

        void clear();

    private:

        typedef unordered_map<int, std::vector<ShardError*> > TrackedErrorMap;
        TrackedErrorMap _errorMap;
    };

}
