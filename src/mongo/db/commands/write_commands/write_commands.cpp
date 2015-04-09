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

#include "mongo/db/commands/write_commands/write_commands.h"

#include "mongo/base/init.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/write_commands/batch_executor.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/d_state.h"

namespace mongo {

    using std::string;
    using std::stringstream;

    namespace {

        MONGO_INITIALIZER(RegisterWriteCommands)(InitializerContext* context) {
            // Leaked intentionally: a Command registers itself when constructed.
            new CmdInsert();
            new CmdUpdate();
            new CmdDelete();
            return Status::OK();
        }

    } // namespace

    WriteCmd::WriteCmd( StringData name, BatchedCommandRequest::BatchType writeType ) :
        Command( name ), _writeType( writeType ) {
    }

    void WriteCmd::redactTooLongLog( mutablebson::Document* cmdObj, StringData fieldName ) {
        namespace mmb = mutablebson;
        mmb::Element root = cmdObj->root();
        mmb::Element field = root.findFirstChildNamed( fieldName );

        // If the cmdObj is too large, it will be a "too big" message given by CachedBSONObj.get()
        if ( !field.ok() ) {
            return;
        }

        // Redact the log if there are more than one documents or operations.
        if ( field.countChildren() > 1 ) {
            field.setValueInt( field.countChildren() );
        }
    }

    // Slaves can't perform writes.
    bool WriteCmd::slaveOk() const { return false; }

    bool WriteCmd::isWriteCommandForConfigServer() const { return false; }

    Status WriteCmd::checkAuthForCommand( ClientBasic* client,
                                          const std::string& dbname,
                                          const BSONObj& cmdObj ) {

        Status status( auth::checkAuthForWriteCommand( AuthorizationSession::get(client),
                _writeType,
                NamespaceString( parseNs( dbname, cmdObj ) ),
                cmdObj ));

        // TODO: Remove this when we standardize GLE reporting from commands
        if ( !status.isOK() ) {
            setLastError( status.code(), status.reason().c_str() );
        }

        return status;
    }

    // Write commands are counted towards their corresponding opcounters, not command opcounters.
    bool WriteCmd::shouldAffectCommandCounter() const { return false; }

    bool WriteCmd::run(OperationContext* txn,
                       const string& dbName,
                       BSONObj& cmdObj,
                       int options,
                       string& errMsg,
                       BSONObjBuilder& result) {
        // Can't be run on secondaries.
        dassert(txn->writesAreReplicated());
        BatchedCommandRequest request( _writeType );
        BatchedCommandResponse response;

        if ( !request.parseBSON( cmdObj, &errMsg ) || !request.isValid( &errMsg ) ) {
            return appendCommandStatus( result, Status( ErrorCodes::FailedToParse, errMsg ) );
        }

        // Note that this is a runCommmand, and therefore, the database and the collection name
        // are in different parts of the grammar for the command. But it's more convenient to
        // work with a NamespaceString. We built it here and replace it in the parsed command.
        // Internally, everything work with the namespace string as opposed to just the
        // collection name.
        NamespaceString nss(dbName, request.getNS());
        request.setNSS(nss);

        StatusWith<WriteConcernOptions> wcStatus = extractWriteConcern(cmdObj);

        if (!wcStatus.isOK()) {
            return appendCommandStatus(result, wcStatus.getStatus());
        }
        txn->setWriteConcern(wcStatus.getValue());

        WriteBatchExecutor writeBatchExecutor(txn,
                                              &globalOpCounters,
                                              lastError.get());

        writeBatchExecutor.executeBatch( request, &response );

        result.appendElements( response.toBSON() );
        return response.getOk();
    }

    Status WriteCmd::explain(OperationContext* txn,
                             const std::string& dbname,
                             const BSONObj& cmdObj,
                             ExplainCommon::Verbosity verbosity,
                             BSONObjBuilder* out) const {
        // For now we only explain update and delete write commands.
        if ( BatchedCommandRequest::BatchType_Update != _writeType &&
             BatchedCommandRequest::BatchType_Delete != _writeType ) {
            return Status( ErrorCodes::IllegalOperation,
                           "Only update and delete write ops can be explained" );
        }

        // Parse the batch request.
        BatchedCommandRequest request( _writeType );
        std::string errMsg;
        if ( !request.parseBSON( cmdObj, &errMsg ) || !request.isValid( &errMsg ) ) {
            return Status( ErrorCodes::FailedToParse, errMsg );
        }

        // Note that this is a runCommmand, and therefore, the database and the collection name
        // are in different parts of the grammar for the command. But it's more convenient to
        // work with a NamespaceString. We built it here and replace it in the parsed command.
        // Internally, everything work with the namespace string as opposed to just the
        // collection name.
        NamespaceString nsString(dbname, request.getNS());
        request.setNSS(nsString);

        // Do the validation of the batch that is shared with non-explained write batches.
        Status isValid = WriteBatchExecutor::validateBatch( request );
        if (!isValid.isOK()) {
            return isValid;
        }

        // Explain must do one additional piece of validation: For now we only explain
        // singleton batches.
        if ( request.sizeWriteOps() != 1u ) {
            return Status( ErrorCodes::InvalidLength,
                           "explained write batches must be of size 1" );
        }

        ScopedTransaction scopedXact(txn, MODE_IX);

        // Get a reference to the singleton batch item (it's the 0th item in the batch).
        BatchItemRef batchItem( &request, 0 );

        if ( BatchedCommandRequest::BatchType_Update == _writeType ) {
            // Create the update request.
            UpdateRequest updateRequest( nsString );
            updateRequest.setQuery( batchItem.getUpdate()->getQuery() );
            updateRequest.setUpdates( batchItem.getUpdate()->getUpdateExpr() );
            updateRequest.setMulti( batchItem.getUpdate()->getMulti() );
            updateRequest.setUpsert( batchItem.getUpdate()->getUpsert() );
            UpdateLifecycleImpl updateLifecycle( true, updateRequest.getNamespaceString() );
            updateRequest.setLifecycle( &updateLifecycle );
            updateRequest.setExplain();

            // Explained updates can yield.
            updateRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);

            OpDebug* debug = &txn->getCurOp()->debug();

            ParsedUpdate parsedUpdate( txn, &updateRequest );
            Status parseStatus = parsedUpdate.parseRequest();
            if ( !parseStatus.isOK() ) {
                return parseStatus;
            }

            // Explains of write commands are read-only, but we take write locks so
            // that timing info is more accurate.
            AutoGetDb autoDb( txn, nsString.db(), MODE_IX );
            Lock::CollectionLock colLock( txn->lockState(), nsString.ns(), MODE_IX );

            // We check the shard version explicitly here rather than using OldClientContext,
            // as Context can do implicit database creation if the db does not exist. We want
            // explain to be a no-op that reports a trivial EOF plan against non-existent dbs
            // or collections.
            ensureShardVersionOKOrThrow( nsString.ns() );

            // Get a pointer to the (possibly NULL) collection.
            Collection* collection = NULL;
            if ( autoDb.getDb() ) {
                collection = autoDb.getDb()->getCollection( nsString.ns() );
            }

            PlanExecutor* rawExec;
            uassertStatusOK(getExecutorUpdate(txn, collection, &parsedUpdate, debug, &rawExec));
            boost::scoped_ptr<PlanExecutor> exec(rawExec);

            // Explain the plan tree.
            Explain::explainStages( exec.get(), verbosity, out );
            return Status::OK();
        }
        else {
            invariant( BatchedCommandRequest::BatchType_Delete == _writeType );

            // Create the delete request.
            DeleteRequest deleteRequest( nsString );
            deleteRequest.setQuery( batchItem.getDelete()->getQuery() );
            deleteRequest.setMulti( batchItem.getDelete()->getLimit() != 1 );
            deleteRequest.setGod( false );
            deleteRequest.setExplain();

            // Explained deletes can yield.
            deleteRequest.setYieldPolicy(PlanExecutor::YIELD_AUTO);

            ParsedDelete parsedDelete(txn, &deleteRequest);
            Status parseStatus = parsedDelete.parseRequest();
            if (!parseStatus.isOK()) {
                return parseStatus;
            }

            // Explains of write commands are read-only, but we take write locks so that timing
            // info is more accurate.
            AutoGetDb autoDb(txn, nsString.db(), MODE_IX);
            Lock::CollectionLock colLock(txn->lockState(), nsString.ns(), MODE_IX);

            // We check the shard version explicitly here rather than using OldClientContext,
            // as Context can do implicit database creation if the db does not exist. We want
            // explain to be a no-op that reports a trivial EOF plan against non-existent dbs
            // or collections.
            ensureShardVersionOKOrThrow( nsString.ns() );

            // Get a pointer to the (possibly NULL) collection.
            Collection* collection = NULL;
            if (autoDb.getDb()) {
                collection = autoDb.getDb()->getCollection(nsString.ns());
            }

            PlanExecutor* rawExec;
            uassertStatusOK(getExecutorDelete(txn, collection, &parsedDelete, &rawExec));
            boost::scoped_ptr<PlanExecutor> exec(rawExec);

            // Explain the plan tree.
            Explain::explainStages(exec.get(), verbosity, out);
            return Status::OK();
        }
    }

    CmdInsert::CmdInsert() :
        WriteCmd( "insert", BatchedCommandRequest::BatchType_Insert ) {
    }

    void CmdInsert::redactForLogging( mutablebson::Document* cmdObj ) {
        redactTooLongLog( cmdObj, StringData( "documents", StringData::LiteralTag() ) );
    }

    void CmdInsert::help( stringstream& help ) const {
        help << "insert documents";
    }

    CmdUpdate::CmdUpdate() :
        WriteCmd( "update", BatchedCommandRequest::BatchType_Update ) {
    }

    void CmdUpdate::redactForLogging( mutablebson::Document* cmdObj ) {
        redactTooLongLog( cmdObj, StringData( "updates", StringData::LiteralTag() ) );
    }

    void CmdUpdate::help( stringstream& help ) const {
        help << "update documents";
    }

    CmdDelete::CmdDelete() :
        WriteCmd( "delete", BatchedCommandRequest::BatchType_Delete ) {
    }

    void CmdDelete::redactForLogging( mutablebson::Document* cmdObj ) {
        redactTooLongLog( cmdObj, StringData( "deletes", StringData::LiteralTag() ) );
    }

    void CmdDelete::help( stringstream& help ) const {
        help << "delete documents";
    }

} // namespace mongo
