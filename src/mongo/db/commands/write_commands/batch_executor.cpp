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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/commands/write_commands/batch_executor.h"

#include <boost/scoped_ptr.hpp>
#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/service_context.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/d_state.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::string;
    using std::vector;

    namespace {

        /**
         * Data structure to safely hold and clean up results of single write operations.
         */
        class WriteOpResult {
            MONGO_DISALLOW_COPYING(WriteOpResult);
        public:
            WriteOpResult() {}

            WriteOpStats& getStats() { return _stats; }

            WriteErrorDetail* getError() { return _error.get(); }
            WriteErrorDetail* releaseError() { return _error.release(); }
            void setError(WriteErrorDetail* error) { _error.reset(error); }

        private:
            WriteOpStats _stats;
            std::auto_ptr<WriteErrorDetail> _error;
        };

    }  // namespace

    // TODO: Determine queueing behavior we want here
    MONGO_EXPORT_SERVER_PARAMETER( queueForMigrationCommit, bool, true );

    using mongoutils::str::stream;

    WriteBatchExecutor::WriteBatchExecutor( OperationContext* txn,
                                            OpCounters* opCounters,
                                            LastError* le ) :
        _txn(txn),
        _opCounters( opCounters ),
        _le( le ),
        _stats( new WriteBatchStats ) {
    }

    static WCErrorDetail* toWriteConcernError( const Status& wcStatus,
                                               const WriteConcernResult& wcResult ) {

        WCErrorDetail* wcError = new WCErrorDetail;

        wcError->setErrCode( wcStatus.code() );
        wcError->setErrMessage( wcStatus.reason() );
        if ( wcResult.wTimedOut )
            wcError->setErrInfo( BSON( "wtimeout" << true ) );

        return wcError;
    }

    static WriteErrorDetail* toWriteError( const Status& status ) {

        WriteErrorDetail* error = new WriteErrorDetail;

        // TODO: Complex transform here?
        error->setErrCode( status.code() );
        error->setErrMessage( status.reason() );

        return error;
    }

    static void toBatchError( const Status& status, BatchedCommandResponse* response ) {
        response->clear();
        response->setErrCode( status.code() );
        response->setErrMessage( status.reason() );
        response->setOk( false );
        dassert( response->isValid(NULL) );
    }

    static void noteInCriticalSection( WriteErrorDetail* staleError ) {
        BSONObjBuilder builder;
        if ( staleError->isErrInfoSet() )
            builder.appendElements( staleError->getErrInfo() );
        builder.append( "inCriticalSection", true );
        staleError->setErrInfo( builder.obj() );
    }

    // static
    Status WriteBatchExecutor::validateBatch( const BatchedCommandRequest& request ) {

        // Validate namespace
        const NamespaceString& nss = request.getNSS();
        if ( !nss.isValid() ) {
            return Status( ErrorCodes::InvalidNamespace,
                           nss.ns() + " is not a valid namespace" );
        }

        // Make sure we can write to the namespace
        Status allowedStatus = userAllowedWriteNS( nss );
        if ( !allowedStatus.isOK() ) {
            return allowedStatus;
        }

        // Validate insert index requests
        // TODO: Push insert index requests through createIndex once all upgrade paths support it
        string errMsg;
        if ( request.isInsertIndexRequest() && !request.isValidIndexRequest( &errMsg ) ) {
            return Status( ErrorCodes::InvalidOptions, errMsg );
        }

        return Status::OK();
    }

    void WriteBatchExecutor::executeBatch( const BatchedCommandRequest& request,
                                           BatchedCommandResponse* response ) {

        // Validate namespace
        Status isValid = validateBatch(request);
        if (!isValid.isOK()) {
            toBatchError( isValid, response );
            return;
        }

        if ( request.sizeWriteOps() == 0u ) {
            toBatchError( Status( ErrorCodes::InvalidLength,
                                  "no write ops were included in the batch" ),
                          response );
            return;
        }

        // Validate batch size
        if ( request.sizeWriteOps() > BatchedCommandRequest::kMaxWriteBatchSize ) {
            toBatchError( Status( ErrorCodes::InvalidLength,
                                  stream() << "exceeded maximum write batch size of "
                                           << BatchedCommandRequest::kMaxWriteBatchSize ),
                          response );
            return;
        }

        //
        // End validation
        //

        const WriteConcernOptions& writeConcern = _txn->getWriteConcern();
        bool silentWC = writeConcern.wMode.empty() && writeConcern.wNumNodes == 0
                        && writeConcern.syncMode == WriteConcernOptions::NONE;

        Timer commandTimer;

        OwnedPointerVector<WriteErrorDetail> writeErrorsOwned;
        vector<WriteErrorDetail*>& writeErrors = writeErrorsOwned.mutableVector();

        OwnedPointerVector<BatchedUpsertDetail> upsertedOwned;
        vector<BatchedUpsertDetail*>& upserted = upsertedOwned.mutableVector();

        //
        // Apply each batch item, possibly bulking some items together in the write lock.
        // Stops on error if batch is ordered.
        //

        bulkExecute( request, &upserted, &writeErrors );

        //
        // Try to enforce the write concern if everything succeeded (unordered or ordered)
        // OR if something succeeded and we're unordered.
        //

        auto_ptr<WCErrorDetail> wcError;
        bool needToEnforceWC = writeErrors.empty()
                               || ( !request.getOrdered()
                                    && writeErrors.size() < request.sizeWriteOps() );

        if ( needToEnforceWC ) {

            _txn->getCurOp()->setMessage( "waiting for write concern" );

            WriteConcernResult res;
            Status status = waitForWriteConcern(
                    _txn,
                    repl::ReplClientInfo::forClient(_txn->getClient()).getLastOp(),
                    &res);

            if ( !status.isOK() ) {
                wcError.reset( toWriteConcernError( status, res ) );
            }
        }

        //
        // Refresh metadata if needed
        //

        bool staleBatch = !writeErrors.empty()
                          && writeErrors.back()->getErrCode() == ErrorCodes::StaleShardVersion;

        if ( staleBatch ) {

            const BatchedRequestMetadata* requestMetadata = request.getMetadata();
            dassert( requestMetadata );

            // Make sure our shard name is set or is the same as what was set previously
            if ( shardingState.setShardName( requestMetadata->getShardName() ) ) {

                //
                // First, we refresh metadata if we need to based on the requested version.
                //

                ChunkVersion latestShardVersion;
                shardingState.refreshMetadataIfNeeded( _txn,
                                                       request.getTargetingNS(),
                                                       requestMetadata->getShardVersion(),
                                                       &latestShardVersion );

                // Report if we're still changing our metadata
                // TODO: Better reporting per-collection
                if ( shardingState.inCriticalMigrateSection() ) {
                    noteInCriticalSection( writeErrors.back() );
                }

                if ( queueForMigrationCommit ) {

                    //
                    // Queue up for migration to end - this allows us to be sure that clients will
                    // not repeatedly try to refresh metadata that is not yet written to the config
                    // server.  Not necessary for correctness.
                    // Exposed as optional parameter to allow testing of queuing behavior with
                    // different network timings.
                    //

                    const ChunkVersion& requestShardVersion = requestMetadata->getShardVersion();

                    //
                    // Only wait if we're an older version (in the current collection epoch) and
                    // we're not write compatible, implying that the current migration is affecting
                    // writes.
                    //

                    if ( requestShardVersion.isOlderThan( latestShardVersion ) &&
                         !requestShardVersion.isWriteCompatibleWith( latestShardVersion ) ) {

                        while ( shardingState.inCriticalMigrateSection() ) {

                            log() << "write request to old shard version "
                                  << requestMetadata->getShardVersion().toString()
                                  << " waiting for migration commit" << endl;

                            shardingState.waitTillNotInCriticalSection( 10 /* secs */);
                        }
                    }
                }
            }
            else {
                // If our shard name is stale, our version must have been stale as well
                dassert( writeErrors.size() == request.sizeWriteOps() );
            }
        }

        //
        // Construct response
        //

        response->setOk( true );

        if ( !silentWC ) {

            if ( upserted.size() ) {
                response->setUpsertDetails( upserted );
            }

            if ( writeErrors.size() ) {
                response->setErrDetails( writeErrors );
            }

            if ( wcError.get() ) {
                response->setWriteConcernError( wcError.release() );
            }

            repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
            const repl::ReplicationCoordinator::Mode replMode = replCoord->getReplicationMode();
            if (replMode != repl::ReplicationCoordinator::modeNone) {
                response->setLastOp(repl::ReplClientInfo::forClient(_txn->getClient()).getLastOp()
                        .getTimestamp());
                if (replMode == repl::ReplicationCoordinator::modeReplSet) {
                    response->setElectionId(replCoord->getElectionId());
                }
            }

            // Set the stats for the response
            response->setN( _stats->numInserted + _stats->numUpserted + _stats->numMatched
                            + _stats->numDeleted );
            if ( request.getBatchType() == BatchedCommandRequest::BatchType_Update )
                response->setNModified( _stats->numModified );
        }

        dassert( response->isValid( NULL ) );
    }

    // Translates write item type to wire protocol op code.
    // Helper for WriteBatchExecutor::applyWriteItem().
    static int getOpCode( BatchedCommandRequest::BatchType writeType ) {
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

    static void buildStaleError( const ChunkVersion& shardVersionRecvd,
                                 const ChunkVersion& shardVersionWanted,
                                 WriteErrorDetail* error ) {

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

    static bool checkShardVersion(OperationContext* txn,
                                  ShardingState* shardingState,
                                  const BatchedCommandRequest& request,
                                  WriteOpResult* result) {

        const NamespaceString& nss = request.getTargetingNSS();
        dassert(txn->lockState()->isCollectionLockedForMode(nss.ns(), MODE_IX));

        ChunkVersion requestShardVersion =
            request.isMetadataSet() && request.getMetadata()->isShardVersionSet() ?
                request.getMetadata()->getShardVersion() : ChunkVersion::IGNORED();

        if ( shardingState->enabled() ) {

            CollectionMetadataPtr metadata = shardingState->getCollectionMetadata( nss.ns() );

            if ( !ChunkVersion::isIgnoredVersion( requestShardVersion ) ) {

                ChunkVersion shardVersion =
                    metadata ? metadata->getShardVersion() : ChunkVersion::UNSHARDED();

                if ( !requestShardVersion.isWriteCompatibleWith( shardVersion ) ) {
                    result->setError(new WriteErrorDetail);
                    buildStaleError(requestShardVersion, shardVersion, result->getError());
                    return false;
                }
            }
        }

        return true;
    }

    static bool checkIsMasterForDatabase(const NamespaceString& ns, WriteOpResult* result) {
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                ns.db())) {
            WriteErrorDetail* errorDetail = new WriteErrorDetail;
            result->setError(errorDetail);
            errorDetail->setErrCode(ErrorCodes::NotMaster);
            errorDetail->setErrMessage("Not primary while writing to " + ns.toString());
            return false;
        }
        return true;
    }

    static void buildUniqueIndexError( const BSONObj& keyPattern,
                                       const BSONObj& indexPattern,
                                       WriteErrorDetail* error ) {
        error->setErrCode( ErrorCodes::CannotCreateIndex );
        string errMsg = stream() << "cannot create unique index over " << indexPattern
                                 << " with shard key pattern " << keyPattern;
        error->setErrMessage( errMsg );
    }

    static bool checkIndexConstraints(OperationContext* txn,
                                      ShardingState* shardingState,
                                      const BatchedCommandRequest& request,
                                      WriteOpResult* result) {

        const NamespaceString& nss = request.getTargetingNSS();
        dassert(txn->lockState()->isCollectionLockedForMode(nss.ns(), MODE_IX));

        if ( !request.isUniqueIndexRequest() )
            return true;

        if ( shardingState->enabled() ) {

            CollectionMetadataPtr metadata = shardingState->getCollectionMetadata( nss.ns() );

            if ( metadata ) {
                ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
                if (!shardKeyPattern.isUniqueIndexCompatible(request.getIndexKeyPattern())) {

                    result->setError(new WriteErrorDetail);
                    buildUniqueIndexError(metadata->getKeyPattern(),
                                          request.getIndexKeyPattern(),
                                          result->getError());

                    return false;
                }
            }
        }

        return true;
    }

    //
    // HELPERS FOR CUROP MANAGEMENT AND GLOBAL STATS
    //

    static void beginCurrentOp( CurOp* currentOp, Client* client, const BatchItemRef& currWrite ) {

        // Execute the write item as a child operation of the current operation.
        // This is not done by out callers

        // Set up the child op with more info
        HostAndPort remote =
            client->hasRemote() ? client->getRemote() : HostAndPort( "0.0.0.0", 0 );
        // TODO Modify CurOp "wrapped" constructor to take an opcode, so calling .reset()
        // is unneeded
        currentOp->reset( remote, getOpCode( currWrite.getRequest()->getBatchType() ) );
        currentOp->ensureStarted();
        currentOp->setNS( currWrite.getRequest()->getNS() );

        currentOp->debug().ns = currentOp->getNS();
        currentOp->debug().op = currentOp->getOp();

        if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Insert ) {
            currentOp->setQuery( currWrite.getDocument() );
            currentOp->debug().query = currWrite.getDocument();
            currentOp->debug().ninserted = 0;
        }
        else if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Update ) {
            currentOp->setQuery( currWrite.getUpdate()->getQuery() );
            currentOp->debug().query = currWrite.getUpdate()->getQuery();
            currentOp->debug().updateobj = currWrite.getUpdate()->getUpdateExpr();
            // Note: debug().nMatched, nModified and nmoved are set internally in update
        }
        else {
            dassert( currWrite.getOpType() == BatchedCommandRequest::BatchType_Delete );
            currentOp->setQuery( currWrite.getDelete()->getQuery() );
            currentOp->debug().query = currWrite.getDelete()->getQuery();
            currentOp->debug().ndeleted = 0;
        }

    }

    void WriteBatchExecutor::incOpStats( const BatchItemRef& currWrite ) {

        if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Insert ) {
            _opCounters->gotInsert();
        }
        else if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Update ) {
            _opCounters->gotUpdate();
        }
        else {
            dassert( currWrite.getOpType() == BatchedCommandRequest::BatchType_Delete );
            _opCounters->gotDelete();
        }
    }

    void WriteBatchExecutor::incWriteStats( const BatchItemRef& currWrite,
                                            const WriteOpStats& stats,
                                            const WriteErrorDetail* error,
                                            CurOp* currentOp ) {

        if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Insert ) {
            _stats->numInserted += stats.n;
            currentOp->debug().ninserted += stats.n;
            if (!error) {
                _le->recordInsert(stats.n);
            }
        }
        else if ( currWrite.getOpType() == BatchedCommandRequest::BatchType_Update ) {
            if ( stats.upsertedID.isEmpty() ) {
                _stats->numMatched += stats.n;
                _stats->numModified += stats.nModified;
            }
            else {
                ++_stats->numUpserted;
            }

            if (!error) {
                _le->recordUpdate( stats.upsertedID.isEmpty() && stats.n > 0,
                        stats.n,
                        stats.upsertedID );
            }
        }
        else {
            dassert( currWrite.getOpType() == BatchedCommandRequest::BatchType_Delete );
            _stats->numDeleted += stats.n;
            if ( !error ) {
                _le->recordDelete( stats.n );
            }
            currentOp->debug().ndeleted += stats.n;
        }

        if (error) {
            _le->setLastError(error->getErrCode(), error->getErrMessage().c_str());
        }
    }

    static void finishCurrentOp( OperationContext* txn,
                                 CurOp* currentOp,
                                 WriteErrorDetail* opError ) {

        currentOp->done();
        int executionTime = currentOp->debug().executionTime = currentOp->totalTimeMillis();
        currentOp->debug().recordStats();
        if (currentOp->getOp() == dbInsert) {
            // This is a wrapped operation, so make sure to count this part of the op
            // SERVER-13339: Properly fix the handling of context in the insert path.
            // Right now it caches client contexts in ExecInsertsState, unlike the
            // update and remove operations.
            currentOp->recordGlobalTime(txn->lockState()->isWriteLocked(),
                                        currentOp->totalTimeMicros());
        }

        if ( opError ) {
            currentOp->debug().exceptionInfo = ExceptionInfo( opError->getErrMessage(),
                                                              opError->getErrCode() );

            LOG(3) << " Caught Assertion in " << opToString( currentOp->getOp() )
                   << ", continuing " << causedBy( opError->getErrMessage() ) << endl;
        }

        bool logAll = logger::globalLogDomain()->shouldLog(logger::LogComponent::kWrite,
                                                           logger::LogSeverity::Debug(1));
        bool logSlow = executionTime
                       > ( serverGlobalParams.slowMS + currentOp->getExpectedLatencyMs() );

        if ( logAll || logSlow ) {
            Locker::LockerInfo lockerInfo;
            txn->lockState()->getLockerInfo(&lockerInfo);

            LOG(0) << currentOp->debug().report(*currentOp, lockerInfo.stats);
        }

        if (currentOp->shouldDBProfile(executionTime)) {
            profile(txn, txn->getCurOp()->getOp());
        }
    }

    // END HELPERS

    //
    // CORE WRITE OPERATIONS (declaration)
    // These functions write to the database and return stats and zero or one of:
    // - page fault
    // - error
    //

    static void singleInsert( OperationContext* txn,
                              const BSONObj& docToInsert,
                              Collection* collection,
                              WriteOpResult* result );

    static void singleCreateIndex( OperationContext* txn,
                                   const BSONObj& indexDesc,
                                   WriteOpResult* result );

    static void multiUpdate( OperationContext* txn,
                             const BatchItemRef& updateItem,
                             WriteOpResult* result );

    static void multiRemove( OperationContext* txn,
                             const BatchItemRef& removeItem,
                             WriteOpResult* result );

    //
    // WRITE EXECUTION
    // In general, the exec* operations manage db lock state and stats before dispatching to the
    // core write operations, which are *only* responsible for performing a write and reporting
    // success or failure.
    //

    /**
     * Representation of the execution state of execInserts.  Used by a single
     * execution of execInserts in a single thread.
     */
    class WriteBatchExecutor::ExecInsertsState {
        MONGO_DISALLOW_COPYING(ExecInsertsState);
    public:
        /**
         * Constructs a new instance, for performing inserts described in "aRequest".
         */
        explicit ExecInsertsState(OperationContext* txn,
                                  const BatchedCommandRequest* aRequest);

        /**
         * Acquires the write lock and client context needed to perform the current write operation.
         * Returns true on success, after which it is safe to use the "context" and "collection"
         * members.  It is safe to call this function if this instance already holds the write lock.
         *
         * On failure, writeLock, context and collection will be NULL/clear.
         */
        bool lockAndCheck(WriteOpResult* result);

        /**
         * Releases the client context and write lock acquired by lockAndCheck.  Safe to call
         * regardless of whether or not this state object currently owns the lock.
         */
        void unlock();

        /**
         * Returns true if this executor has the lock on the target database.
         */
        bool hasLock() { return _writeLock.get(); }

        /**
         * Gets the lock-holding object.  Only valid if hasLock().
         */
        Lock::DBLock& getLock() { return *_writeLock; }

        /**
         * Gets the target collection for the batch operation.  Value is undefined
         * unless hasLock() is true.
         */
        Collection* getCollection() { return _collection; }

        OperationContext* txn;

        // Request object describing the inserts.
        const BatchedCommandRequest* request;

        // Index of the current insert operation to perform.
        size_t currIndex;

        // Translation of insert documents in "request" into insert-ready forms.  This vector has a
        // correspondence with elements of the "request", and "currIndex" is used to
        // index both.
        std::vector<StatusWith<BSONObj> > normalizedInserts;

    private:
        bool _lockAndCheckImpl(WriteOpResult* result, bool intentLock);

        ScopedTransaction _transaction;
        // Guard object for the write lock on the target database.
        scoped_ptr<Lock::DBLock> _writeLock;
        scoped_ptr<Lock::CollectionLock> _collLock;

        // Context object on the target database.  Must appear after writeLock, so that it is
        // destroyed in proper order.
        scoped_ptr<OldClientContext> _context;

        // Target collection.
        Collection* _collection;
    };

    void WriteBatchExecutor::bulkExecute( const BatchedCommandRequest& request,
                                          std::vector<BatchedUpsertDetail*>* upsertedIds,
                                          std::vector<WriteErrorDetail*>* errors ) {
        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (request.shouldBypassValidation()) {
            maybeDisableValidation.emplace(_txn);
        }

        if ( request.getBatchType() == BatchedCommandRequest::BatchType_Insert ) {
            execInserts( request, errors );
        }
        else if ( request.getBatchType() == BatchedCommandRequest::BatchType_Update ) {
            for ( size_t i = 0; i < request.sizeWriteOps(); i++ ) {

                if ( i + 1 == request.sizeWriteOps() ) {
                    setupSynchronousCommit( _txn );
                }

                WriteErrorDetail* error = NULL;
                BSONObj upsertedId;
                execUpdate( BatchItemRef( &request, i ), &upsertedId, &error );

                if ( !upsertedId.isEmpty() ) {
                    BatchedUpsertDetail* batchUpsertedId = new BatchedUpsertDetail;
                    batchUpsertedId->setIndex( i );
                    batchUpsertedId->setUpsertedID( upsertedId );
                    upsertedIds->push_back( batchUpsertedId );
                }

                if ( error ) {
                    errors->push_back( error );
                    if ( request.getOrdered() )
                        break;
                }
            }
        }
        else {
            dassert( request.getBatchType() == BatchedCommandRequest::BatchType_Delete );
            for ( size_t i = 0; i < request.sizeWriteOps(); i++ ) {

                if ( i + 1 == request.sizeWriteOps() ) {
                    setupSynchronousCommit( _txn );
                }

                WriteErrorDetail* error = NULL;
                execRemove( BatchItemRef( &request, i ), &error );

                if ( error ) {
                    errors->push_back( error );
                    if ( request.getOrdered() )
                        break;
                }
            }
        }

        // Fill in stale version errors for unordered batches (update/delete can't do this on own)
        if ( !errors->empty() && !request.getOrdered() ) {

            const WriteErrorDetail* finalError = errors->back();

            if ( finalError->getErrCode() == ErrorCodes::StaleShardVersion ) {
                for ( size_t i = finalError->getIndex() + 1; i < request.sizeWriteOps(); i++ ) {
                    WriteErrorDetail* dupStaleError = new WriteErrorDetail;
                    finalError->cloneTo( dupStaleError );
                    errors->push_back( dupStaleError );
                }
            }
        }
    }

    // Goes over the request and preprocesses normalized versions of all the inserts in the request
    static void normalizeInserts( const BatchedCommandRequest& request,
                                  vector<StatusWith<BSONObj> >* normalizedInserts ) {

        normalizedInserts->reserve(request.sizeWriteOps());
        for ( size_t i = 0; i < request.sizeWriteOps(); ++i ) {
            BSONObj insertDoc = request.getInsertRequest()->getDocumentsAt( i );
            StatusWith<BSONObj> normalInsert = fixDocumentForInsert( insertDoc );
            normalizedInserts->push_back( normalInsert );
            if ( request.getOrdered() && !normalInsert.isOK() )
                break;
        }
    }

    void WriteBatchExecutor::execInserts( const BatchedCommandRequest& request,
                                          std::vector<WriteErrorDetail*>* errors ) {

        // Theory of operation:
        //
        // Instantiates an ExecInsertsState, which represents all of the state involved in the batch
        // insert execution algorithm.  Most importantly, encapsulates the lock state.
        //
        // Every iteration of the loop in execInserts() processes one document insertion, by calling
        // insertOne() exactly once for a given value of state.currIndex.
        //
        // If the ExecInsertsState indicates that the requisite write locks are not held, insertOne
        // acquires them and performs lock-acquisition-time checks.  However, on non-error
        // execution, it does not release the locks.  Therefore, the yielding logic in the while
        // loop in execInserts() is solely responsible for lock release in the non-error case.
        //
        // Internally, insertOne loops performing the single insert until it completes without a
        // PageFaultException, or until it fails with some kind of error.  Errors are mostly
        // propagated via the request->error field, but DBExceptions or std::exceptions may escape,
        // particularly on operation interruption.  These kinds of errors necessarily prevent
        // further insertOne calls, and stop the batch.  As a result, the only expected source of
        // such exceptions are interruptions.
        ExecInsertsState state(_txn, &request);
        normalizeInserts(request, &state.normalizedInserts);

        ShardedConnectionInfo* info = ShardedConnectionInfo::get(false);
        if (info) {
            if (request.isMetadataSet() && request.getMetadata()->isShardVersionSet()) {
                info->setVersion(request.getTargetingNS(),
                                 request.getMetadata()->getShardVersion());
            }
            else {
                info->setVersion(request.getTargetingNS(), ChunkVersion::IGNORED());
            }
        }

        // Yield frequency is based on the same constants used by PlanYieldPolicy.
        ElapsedTracker elapsedTracker(internalQueryExecYieldIterations,
                                      internalQueryExecYieldPeriodMS);

        for (state.currIndex = 0;
             state.currIndex < state.request->sizeWriteOps();
             ++state.currIndex) {

            if (state.currIndex + 1 == state.request->sizeWriteOps()) {
                setupSynchronousCommit(_txn);
            }

            if (elapsedTracker.intervalHasElapsed()) {
                // Yield between inserts.
                if (state.hasLock()) {
                    // Release our locks. They get reacquired when insertOne() calls
                    // ExecInsertsState::lockAndCheck(). Since the lock manager guarantees FIFO
                    // queues waiting on locks, there is no need to explicitly sleep or give up
                    // control of the processor here.
                    state.unlock();

                    // This releases any storage engine held locks/snapshots.
                    _txn->recoveryUnit()->commitAndRestart();
                }

                _txn->checkForInterrupt();
                elapsedTracker.resetLastTime();
            }

            WriteErrorDetail* error = NULL;
            execOneInsert(&state, &error);
            if (error) {
                errors->push_back(error);
                error->setIndex(state.currIndex);
                if (request.getOrdered())
                    return;
            }
        }
    }

    void WriteBatchExecutor::execUpdate( const BatchItemRef& updateItem,
                                         BSONObj* upsertedId,
                                         WriteErrorDetail** error ) {

        // BEGIN CURRENT OP
        CurOp currentOp(_txn->getClient());
        beginCurrentOp( &currentOp, _txn->getClient(), updateItem );
        incOpStats( updateItem );

        ShardedConnectionInfo* info = ShardedConnectionInfo::get(false);
        if (info) {
            auto rootRequest = updateItem.getRequest();
            if (!updateItem.getUpdate()->getMulti() &&
                    rootRequest->isMetadataSet() &&
                    rootRequest->getMetadata()->isShardVersionSet()) {
                info->setVersion(rootRequest->getTargetingNS(),
                                 rootRequest->getMetadata()->getShardVersion());
            }
            else {
                info->setVersion(rootRequest->getTargetingNS(), ChunkVersion::IGNORED());
            }
        }

        WriteOpResult result;

        multiUpdate( _txn, updateItem, &result );

        if ( !result.getStats().upsertedID.isEmpty() ) {
            *upsertedId = result.getStats().upsertedID;
        }
        // END CURRENT OP
        incWriteStats( updateItem, result.getStats(), result.getError(), &currentOp );
        finishCurrentOp( _txn, &currentOp, result.getError() );

        // End current transaction and release snapshot.
        _txn->recoveryUnit()->commitAndRestart();

        if ( result.getError() ) {
            result.getError()->setIndex( updateItem.getItemIndex() );
            *error = result.releaseError();
        }
    }

    void WriteBatchExecutor::execRemove( const BatchItemRef& removeItem,
                                         WriteErrorDetail** error ) {

        // Removes are similar to updates, but page faults are handled externally

        // BEGIN CURRENT OP
        CurOp currentOp(_txn->getClient());
        beginCurrentOp( &currentOp, _txn->getClient(), removeItem );
        incOpStats( removeItem );

        ShardedConnectionInfo* info = ShardedConnectionInfo::get(false);
        if (info) {
            auto rootRequest = removeItem.getRequest();
            if (removeItem.getDelete()->getLimit() == 1 &&
                    rootRequest->isMetadataSet() &&
                    rootRequest->getMetadata()->isShardVersionSet()) {
                info->setVersion(rootRequest->getTargetingNS(),
                                 rootRequest->getMetadata()->getShardVersion());
            }
            else {
                info->setVersion(rootRequest->getTargetingNS(), ChunkVersion::IGNORED());
            }
        }

        WriteOpResult result;

        multiRemove( _txn, removeItem, &result );

        // END CURRENT OP
        incWriteStats( removeItem, result.getStats(), result.getError(), &currentOp );
        finishCurrentOp( _txn, &currentOp, result.getError() );

        // End current transaction and release snapshot.
        _txn->recoveryUnit()->commitAndRestart();

        if ( result.getError() ) {
            result.getError()->setIndex( removeItem.getItemIndex() );
            *error = result.releaseError();
        }
    }

    //
    // IN-DB-LOCK CORE OPERATIONS
    //

    WriteBatchExecutor::ExecInsertsState::ExecInsertsState(OperationContext* txn,
                                                           const BatchedCommandRequest* aRequest) :
        txn(txn),
        request(aRequest),
        currIndex(0),
        _transaction(txn, MODE_IX),
        _collection(NULL) {
    }

    bool WriteBatchExecutor::ExecInsertsState::_lockAndCheckImpl(WriteOpResult* result,
                                                                 bool intentLock) {
        if (hasLock()) {
            // TODO: OldClientContext legacy, needs to be removed
            txn->getCurOp()->enter(_context->ns(),
                                   _context->db() ? _context->db()->getProfilingLevel() : 0);
            return true;
        }

        if (request->isInsertIndexRequest())
            intentLock = false; // can't build indexes in intent mode

        invariant(!_context.get());
        const NamespaceString& nss = request->getNSS();
        _collLock.reset(); // give up locks if any
        _writeLock.reset();
        _writeLock.reset(new Lock::DBLock(txn->lockState(),
                                          nss.db(),
                                          intentLock ? MODE_IX : MODE_X));
        if (intentLock && dbHolder().get(txn, nss.db()) == NULL) {
            // Ensure exclusive lock in case the database doesn't yet exist
            _writeLock.reset();
            _writeLock.reset(new Lock::DBLock(txn->lockState(),
                                              nss.db(),
                                              MODE_X));
            intentLock = false;
        }
        _collLock.reset(new Lock::CollectionLock(txn->lockState(),
                                                 nss.ns(),
                                                 intentLock ? MODE_IX : MODE_X));
        if (!checkIsMasterForDatabase(nss, result)) {
            return false;
        }
        if (!checkShardVersion(txn, &shardingState, *request, result)) {
            return false;
        }
        if (!checkIndexConstraints(txn, &shardingState, *request, result)) {
            return false;
        }

        _context.reset();
        _context.reset(new OldClientContext(txn, nss, false));

        Database* database = _context->db();
        dassert(database);
        _collection = database->getCollection(request->getTargetingNS());
        if (!_collection) {
            if (intentLock) {
                // try again with full X lock.
                unlock();
                return _lockAndCheckImpl(result, false);
            }

            WriteUnitOfWork wunit (txn);
            // Implicitly create if it doesn't exist
            _collection = database->createCollection(txn, request->getTargetingNS());
            if (!_collection) {
                result->setError(
                        toWriteError(Status(ErrorCodes::InternalError,
                                            "could not create collection " +
                                            request->getTargetingNS())));
                return false;
            }
            wunit.commit();
        }
        return true;
    }

    bool WriteBatchExecutor::ExecInsertsState::lockAndCheck(WriteOpResult* result) {
        if (_lockAndCheckImpl(result, true))
            return true;
        unlock();
        return false;
    }

    void WriteBatchExecutor::ExecInsertsState::unlock() {
        _collection = NULL;
        _context.reset();
        _collLock.reset();
        _writeLock.reset();
    }

    static void insertOne(WriteBatchExecutor::ExecInsertsState* state, WriteOpResult* result) {
        // we have to be top level so we can retry
        invariant(!state->txn->lockState()->inAWriteUnitOfWork() );
        invariant(state->currIndex < state->normalizedInserts.size());

        const StatusWith<BSONObj>& normalizedInsert(state->normalizedInserts[state->currIndex]);

        if (!normalizedInsert.isOK()) {
            result->setError(toWriteError(normalizedInsert.getStatus()));
            return;
        }

        const BSONObj& insertDoc = normalizedInsert.getValue().isEmpty() ?
            state->request->getInsertRequest()->getDocumentsAt( state->currIndex ) :
            normalizedInsert.getValue();

        int attempt = 0;
        while (true) {
            try {
                if (!state->request->isInsertIndexRequest()) {
                    if (state->lockAndCheck(result)) {
                        singleInsert(state->txn, insertDoc, state->getCollection(), result);
                    }
                }
                else {
                    singleCreateIndex(state->txn, insertDoc, result);
                }
                break;
            }
            catch ( const WriteConflictException& wce ) {
                state->unlock();
                state->txn->getCurOp()->debug().writeConflicts++;
                state->txn->recoveryUnit()->commitAndRestart();
                WriteConflictException::logAndBackoff( attempt++,
                                                       "insert",
                                                       state->getCollection() ?
                                                       state->getCollection()->ns().ns() :
                                                       "index" );
            }
            catch (const StaleConfigException& staleExcep) {
                result->setError(new WriteErrorDetail);
                result->getError()->setErrCode(ErrorCodes::StaleShardVersion);
                buildStaleError(staleExcep.getVersionReceived(),
                                staleExcep.getVersionWanted(),
                                result->getError());
                break;
            }
            catch (const DBException& ex) {
                Status status(ex.toStatus());
                if (ErrorCodes::isInterruption(status.code()))
                    throw;
                result->setError(toWriteError(status));
                break;
            }
        }

        // Errors release the write lock, as a matter of policy.
        if (result->getError()) {
            state->txn->recoveryUnit()->commitAndRestart();
            state->unlock();
        }
    }

    void WriteBatchExecutor::execOneInsert(ExecInsertsState* state, WriteErrorDetail** error) {
        BatchItemRef currInsertItem(state->request, state->currIndex);
        CurOp currentOp(_txn->getClient());
        beginCurrentOp( &currentOp, _txn->getClient(), currInsertItem );
        incOpStats(currInsertItem);

        WriteOpResult result;
        insertOne(state, &result);

        incWriteStats(currInsertItem,
                      result.getStats(),
                      result.getError(),
                      &currentOp);
        finishCurrentOp(_txn, &currentOp, result.getError());

        if (result.getError()) {
            *error = result.releaseError();
        }
    }

    /**
     * Perform a single insert into a collection.  Requires the insert be preprocessed and the
     * collection already has been created.
     *
     * Might fault or error, otherwise populates the result.
     */
    static void singleInsert( OperationContext* txn,
                              const BSONObj& docToInsert,
                              Collection* collection,
                              WriteOpResult* result ) {

        const string& insertNS = collection->ns().ns();
        invariant(txn->lockState()->isCollectionLockedForMode(insertNS, MODE_IX));

        WriteUnitOfWork wunit(txn);
        StatusWith<RecordId> status = collection->insertDocument( txn, docToInsert, true );

        if ( !status.isOK() ) {
            result->setError(toWriteError(status.getStatus()));
        }
        else {
            result->getStats().n = 1;
            wunit.commit();
        }
    }

    /**
     * Perform a single index creation on a collection.  Requires the index descriptor be
     * preprocessed.
     *
     * Might fault or error, otherwise populates the result.
     */
    static void singleCreateIndex(OperationContext* txn,
                                  const BSONObj& indexDesc,
                                  WriteOpResult* result) {

        BSONElement nsElement = indexDesc["ns"];
        uassert(ErrorCodes::NoSuchKey,
                "Missing \"ns\" field in index description",
                !nsElement.eoo());
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Expected \"ns\" field of index description to be a " "string, "
                "but found a " << typeName(nsElement.type()),
                nsElement.type() == String);
        const NamespaceString ns(nsElement.valueStringData());
        BSONObjBuilder cmdBuilder;
        cmdBuilder << "createIndexes" << ns.coll();
        cmdBuilder << "indexes" << BSON_ARRAY(indexDesc);
        BSONObj cmd = cmdBuilder.done();
        Command* createIndexesCmd = Command::findCommand("createIndexes");
        invariant(createIndexesCmd);
        std::string errmsg;
        BSONObjBuilder resultBuilder;
        const bool success = createIndexesCmd->run(
                txn,
                ns.db().toString(),
                cmd,
                0,
                errmsg,
                resultBuilder);
        Command::appendCommandStatus(resultBuilder, success, errmsg);
        BSONObj cmdResult = resultBuilder.done();
        uassertStatusOK(Command::getStatusFromCommandResult(cmdResult));
        result->getStats().n =
            cmdResult["numIndexesAfter"].numberInt() - cmdResult["numIndexesBefore"].numberInt();
    }

    static void multiUpdate( OperationContext* txn,
                             const BatchItemRef& updateItem,
                             WriteOpResult* result ) {

        const NamespaceString nsString(updateItem.getRequest()->getNS());
        const bool isMulti = updateItem.getUpdate()->getMulti();
        UpdateRequest request(nsString);
        request.setQuery(updateItem.getUpdate()->getQuery());
        request.setUpdates(updateItem.getUpdate()->getUpdateExpr());
        request.setMulti(isMulti);
        request.setUpsert(updateItem.getUpdate()->getUpsert());
        UpdateLifecycleImpl updateLifecycle(true, request.getNamespaceString());
        request.setLifecycle(&updateLifecycle);

        // Updates from the write commands path can yield.
        request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

        int attempt = 0;
        bool createCollection = false;
        for ( int fakeLoop = 0; fakeLoop < 1; fakeLoop++ ) {

            ParsedUpdate parsedUpdate(txn, &request);
            Status status = parsedUpdate.parseRequest();
            if (!status.isOK()) {
                result->setError(toWriteError(status));
                return;
            }

            if ( createCollection ) {
                MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                    ScopedTransaction transaction(txn, MODE_IX);
                    Lock::DBLock lk(txn->lockState(), nsString.db(), MODE_X);
                    OldClientContext ctx(txn, nsString.ns(), false /* don't check version */);

                    if (!checkIsMasterForDatabase(nsString, result)) {
                        return;
                    }

                    Database* db = ctx.db();
                    if ( db->getCollection( nsString.ns() ) ) {
                        // someone else beat us to it
                    }
                    else {
                        WriteUnitOfWork wuow(txn);
                        uassertStatusOK(userCreateNS(txn, db, nsString.ns(), BSONObj()));
                        wuow.commit();
                    }
                } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "update", nsString.ns());
            }

            ///////////////////////////////////////////
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), nsString.db(), MODE_IX);
            Lock::CollectionLock colLock(txn->lockState(),
                                         nsString.ns(),
                                         parsedUpdate.isIsolated() ? MODE_X : MODE_IX);
            ///////////////////////////////////////////

            if (!checkIsMasterForDatabase(nsString, result)) {
                return;
            }

            if (!checkShardVersion(txn, &shardingState, *updateItem.getRequest(), result))
                return;

            Database* const db = dbHolder().get(txn, nsString.db());

            if (db == NULL) {
                if (createCollection) {
                    // we raced with some, accept defeat
                    result->getStats().nModified = 0;
                    result->getStats().n = 0;
                    return;
                }

                // Database not yet created
                if (!request.isUpsert()) {
                    // not an upsert, no database, nothing to do
                    result->getStats().nModified = 0;
                    result->getStats().n = 0;
                    return;
                }

                //  upsert, don't try to get a context as no MODE_X lock is held
                fakeLoop = -1;
                createCollection = true;
                continue;
            }

            OldClientContext ctx(txn, nsString.ns(), false /* don't check version */);
            Collection* collection = db->getCollection(nsString.ns());

            if ( collection == NULL ) {
                if ( createCollection ) {
                    // we raced with some, accept defeat
                    result->getStats().nModified = 0;
                    result->getStats().n = 0;
                    return;
                }

                if ( !request.isUpsert() ) {
                    // not an upsert, no collection, nothing to do
                    result->getStats().nModified = 0;
                    result->getStats().n = 0;
                    return;
                }

                // upsert, mark that we should create collection
                fakeLoop = -1;
                createCollection = true;
                continue;
            }

            OpDebug* debug = &txn->getCurOp()->debug();

            try {
                invariant(collection);
                PlanExecutor* rawExec;
                uassertStatusOK(getExecutorUpdate(txn, collection, &parsedUpdate, debug, &rawExec));
                boost::scoped_ptr<PlanExecutor> exec(rawExec);

                uassertStatusOK(exec->executePlan());
                UpdateResult res = UpdateStage::makeUpdateResult(exec.get(), debug);

                const long long numDocsModified = res.numDocsModified;
                const long long numMatched = res.numMatched;
                const BSONObj resUpsertedID = res.upserted;

                // We have an _id from an insert
                const bool didInsert = !resUpsertedID.isEmpty();

                result->getStats().nModified = didInsert ? 0 : numDocsModified;
                result->getStats().n = didInsert ? 1 : numMatched;
                result->getStats().upsertedID = resUpsertedID;
            }
            catch ( const WriteConflictException& dle ) {
                debug->writeConflicts++;
                if ( isMulti ) {
                    log() << "Had WriteConflict during multi update, aborting";
                    throw;
                }

                createCollection = false;
                // RESTART LOOP
                fakeLoop = -1;
                txn->recoveryUnit()->commitAndRestart();

                WriteConflictException::logAndBackoff( attempt++, "update", nsString.ns() );
            }
            catch (const StaleConfigException& staleExcep) {
                result->setError(new WriteErrorDetail);
                result->getError()->setErrCode(ErrorCodes::StaleShardVersion);
                buildStaleError(staleExcep.getVersionReceived(),
                                staleExcep.getVersionWanted(),
                                result->getError());
            }
            catch (const DBException& ex) {
                Status status = ex.toStatus();
                if (ErrorCodes::isInterruption(status.code())) {
                    throw;
                }
                result->setError(toWriteError(status));
            }
        }
    }

    /**
     * Perform a remove operation, which might remove multiple documents.  Dispatches to remove code
     * currently to do most of this.
     *
     * Might fault or error, otherwise populates the result.
     */
    static void multiRemove( OperationContext* txn,
                             const BatchItemRef& removeItem,
                             WriteOpResult* result ) {

        const NamespaceString& nss = removeItem.getRequest()->getNSS();
        DeleteRequest request(nss);
        request.setQuery( removeItem.getDelete()->getQuery() );
        request.setMulti( removeItem.getDelete()->getLimit() != 1 );
        request.setGod( false );

        // Deletes running through the write commands path can yield.
        request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

        int attempt = 1;
        while ( 1 ) {
            try {

                ParsedDelete parsedDelete(txn, &request);
                Status status = parsedDelete.parseRequest();
                if (!status.isOK()) {
                    result->setError(toWriteError(status));
                    return;
                }

                ScopedTransaction scopedXact(txn, MODE_IX);
                AutoGetDb autoDb(txn, nss.db(), MODE_IX);
                if (!autoDb.getDb()) {
                    break;
                }

                Lock::CollectionLock collLock(txn->lockState(),
                                              nss.ns(),
                                              parsedDelete.isIsolated() ? MODE_X : MODE_IX);

                // getExecutorDelete() also checks if writes are allowed.
                if (!checkIsMasterForDatabase(nss, result)) {
                    return;
                }
                // Check version once we're locked

                if (!checkShardVersion(txn, &shardingState, *removeItem.getRequest(), result)) {
                    // Version error
                    return;
                }

                // Context once we're locked, to set more details in currentOp()
                // TODO: better constructor?
                OldClientContext ctx(txn, nss.ns(), false /* don't check version */);

                PlanExecutor* rawExec;
                uassertStatusOK(getExecutorDelete(txn,
                                                  ctx.db()->getCollection(nss),
                                                  &parsedDelete,
                                                  &rawExec));
                boost::scoped_ptr<PlanExecutor> exec(rawExec);

                // Execute the delete and retrieve the number deleted.
                uassertStatusOK(exec->executePlan());
                result->getStats().n = DeleteStage::getNumDeleted(exec.get());

                break;
            }
            catch ( const WriteConflictException& dle ) {
                txn->getCurOp()->debug().writeConflicts++;
                WriteConflictException::logAndBackoff( attempt++, "delete", nss.ns() );
            }
            catch (const StaleConfigException& staleExcep) {
                result->setError(new WriteErrorDetail);
                result->getError()->setErrCode(ErrorCodes::StaleShardVersion);
                buildStaleError(staleExcep.getVersionReceived(),
                                staleExcep.getVersionWanted(),
                                result->getError());
                return;
            }
            catch ( const DBException& ex ) {
                Status status = ex.toStatus();
                if (ErrorCodes::isInterruption(status.code())) {
                    throw;
                }
                result->setError(toWriteError(status));
                return;
            }
        }
    }

} // namespace mongo
