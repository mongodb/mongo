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

namespace mongo {

    using std::auto_ptr;

    WriteBatchExecutor::WriteBatchExecutor( Client* client, OpCounters* opCounters, LastError* le ) :
            _client( client ), _opCounters( opCounters ), _le( le ) {
    }

    void WriteBatchExecutor::executeBatch( const BatchedCommandRequest& request,
                                           BatchedCommandResponse* response ) {

        Timer commandTimer;

        WriteStats stats;
        auto_ptr<BatchedErrorDetail> error( new BatchedErrorDetail );
        bool batchSuccess = true;

        // Apply batch ops
        size_t numBatchOps = request.sizeWriteOps();
        for ( size_t i = 0; i < numBatchOps; i++ ) {
            if ( !applyWriteItem( request, i, &stats, error.get() ) ) {

                // Batch item failed
                error->setIndex( static_cast<int>( i ) );
                response->addToErrDetails( error.release() );
                batchSuccess = false;

                if ( !request.getContinueOnError() ) break;

                error.reset( new BatchedErrorDetail );
            }
        }

        //int batchTime = commandTimer.millisReset();

        // Do write concern
        string errMsg;
        BSONObjBuilder wcResultsB;
        if ( !waitForWriteConcern( request.getWriteConcern(),
                                   !batchSuccess,
                                   &wcResultsB,
                                   &errMsg ) ) {
            response->setErrCode( 99999 );
            response->setErrInfo( wcResultsB.obj() );
            response->setErrMessage( errMsg );
        }

        // int wcTime = commandTimer.millisReset();

        // Only one of these is set
        response->setN( stats.numUpdated + stats.numInserted + stats.numDeleted );
        response->setUpserted( stats.numUpserted );

        bool onlyOpErrors = !response->isErrCodeSet() && response->isErrDetailsSet()
                            && response->sizeErrDetails() > 0;
        if ( numBatchOps == 1 && onlyOpErrors ) {
            // Promote single error
            const BatchedErrorDetail* error = response->getErrDetailsAt( 0 );
            response->setErrCode( error->getErrCode() );
            if ( error->isErrInfoSet() ) response->setErrInfo( error->getErrInfo() );
            response->setErrMessage( error->getErrMessage() );
            response->unsetErrDetails();
            error = NULL;
        }
        else if ( numBatchOps > 1 && onlyOpErrors ) {
            response->setErrCode( 99999 );
            response->setErrMessage( "batch op errors occurred" );
        }

        response->setOk( !response->isErrCodeSet() );
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

    bool WriteBatchExecutor::applyWriteItem( const BatchedCommandRequest& request,
                                             int index,
                                             WriteStats* stats,
                                             BatchedErrorDetail* error ) {
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

                // TODO Modify CurOp "wrapped" constructor to take an opcode, so calling .reset()
                // is unneeded
                childOp.reset( _client->getRemote(), getOpCode( request.getBatchType() ) );

                childOp.ensureStarted();
                OpDebug& opDebug = childOp.debug();
                opDebug.ns = ns;
                {
                    Client::WriteContext ctx( ns );

                    switch ( request.getBatchType() ) {
                    case BatchedCommandRequest::BatchType_Insert:
                        opSuccess =
                                applyInsert( ns,
                                             request.getInsertRequest()->getDocumentsAt( index ),
                                             &childOp,
                                             stats,
                                             error );
                        break;
                    case BatchedCommandRequest::BatchType_Update:
                        opSuccess = applyUpdate( ns,
                                                 *request.getUpdateRequest()->getUpdatesAt( index ),
                                                 &childOp,
                                                 stats,
                                                 error );
                        break;
                    default:
                        dassert( request.getBatchType() ==
                                BatchedCommandRequest::BatchType_Delete );
                        opSuccess = applyDelete( ns,
                                                 *request.getDeleteRequest()->getDeletesAt( index ),
                                                 &childOp,
                                                 stats,
                                                 error );
                        break;
                    }
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

    bool WriteBatchExecutor::applyInsert( const string& ns,
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

    bool WriteBatchExecutor::applyUpdate( const string& ns,
                                          const BatchedUpdateDocument& updateOp,
                                          CurOp* currentOp,
                                          WriteStats* stats,
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

        bool resExisting = false;
        long long resNum = 0;
        OID resUpserted = OID();
        try {

            const NamespaceString requestNs( ns );
            UpdateRequest request( requestNs );

            request.setQuery( queryObj );
            request.setUpdates( updateObj );
            request.setUpsert( upsert );
            request.setMulti( multi );
            request.setUpdateOpLog();

            UpdateResult res = update( request, &opDebug );

            resExisting = res.existing;
            resNum = res.numMatched;
            resUpserted = res.upserted;

            stats->numUpdated += !resUpserted.isSet() ? resNum : 0;
            stats->numUpserted += resUpserted.isSet() ? 1 : 0;
        }
        catch ( const UserException& ex ) {
            opDebug.exceptionInfo = ex.getInfo();
            toBatchedError( ex, error );
            return false;
        }

        _le->recordUpdate( resExisting, resNum, resUpserted );

        return true;
    }

    bool WriteBatchExecutor::applyDelete( const string& ns,
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
            n = deleteObjects( ns.c_str(), queryObj, // ns, query
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
