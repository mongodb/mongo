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
#include "mongo/db/commands/write_commands/batch_executor.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/delete_executor.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/update_executor.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

    namespace {

        MONGO_INITIALIZER(RegisterWriteCommands)(InitializerContext* context) {
            // Leaked intentionally: a Command registers itself when constructed.
            new CmdInsert();
            new CmdUpdate();
            new CmdDelete();
            return Status::OK();
        }

    } // namespace

    WriteCmd::WriteCmd( const StringData& name, BatchedCommandRequest::BatchType writeType ) :
        Command( name ), _writeType( writeType ) {
    }

    void WriteCmd::redactTooLongLog( mutablebson::Document* cmdObj, const StringData& fieldName ) {
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

        Status status( auth::checkAuthForWriteCommand( client->getAuthorizationSession(),
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
                       BSONObjBuilder& result,
                       bool fromRepl) {

        // Can't be run on secondaries (logTheOp() == false, slaveOk() == false).
        dassert( !fromRepl );
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
        request.setNS(nss.ns());

        BSONObj defaultWriteConcern =
                repl::getGlobalReplicationCoordinator()->getGetLastErrorDefault();

        WriteBatchExecutor writeBatchExecutor(txn,
                                              defaultWriteConcern,
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
        request.setNS(nsString.ns());

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

        // Get a reference to the singleton batch item (it's the 0th item in the batch).
        BatchItemRef batchItem( &request, 0 );

        if ( BatchedCommandRequest::BatchType_Update == _writeType ) {
            // Create the update request.
            UpdateRequest updateRequest( txn, nsString );
            updateRequest.setQuery( batchItem.getUpdate()->getQuery() );
            updateRequest.setUpdates( batchItem.getUpdate()->getUpdateExpr() );
            updateRequest.setMulti( batchItem.getUpdate()->getMulti() );
            updateRequest.setUpsert( batchItem.getUpdate()->getUpsert() );
            updateRequest.setUpdateOpLog( true );
            UpdateLifecycleImpl updateLifecycle( true, updateRequest.getNamespaceString() );
            updateRequest.setLifecycle( &updateLifecycle );
            updateRequest.setExplain();

            // Use the request to create an UpdateExecutor, and from it extract the
            // plan tree which will be used to execute this update.
            UpdateExecutor updateExecutor( &updateRequest, &txn->getCurOp()->debug() );
            Status prepStatus = updateExecutor.prepare();
            if ( !prepStatus.isOK() ) {
                return prepStatus;
            }

            // Explains of write commands are read-only, but we take a write lock so that timing
            // info is more accurate.
            Client::WriteContext ctx( txn, nsString );

            Status prepInLockStatus = updateExecutor.prepareInLock( ctx.ctx().db() );
            if ( !prepInLockStatus.isOK() ) {
                return prepInLockStatus;
            }

            PlanExecutor* exec = updateExecutor.getPlanExecutor();
            const ScopedExecutorRegistration safety( exec );

            // Explain the plan tree.
            return Explain::explainStages( txn, exec, verbosity, out );
        }
        else {
            invariant( BatchedCommandRequest::BatchType_Delete == _writeType );

            // Create the delete request.
            DeleteRequest deleteRequest( txn, nsString );
            deleteRequest.setQuery( batchItem.getDelete()->getQuery() );
            deleteRequest.setMulti( batchItem.getDelete()->getLimit() != 1 );
            deleteRequest.setUpdateOpLog(true);
            deleteRequest.setGod( false );
            deleteRequest.setExplain();

            // Use the request to create a DeleteExecutor, and from it extract the
            // plan tree which will be used to execute this update.
            DeleteExecutor deleteExecutor( &deleteRequest );
            Status prepStatus = deleteExecutor.prepare();
            if ( !prepStatus.isOK() ) {
                return prepStatus;
            }

            // Explains of write commands are read-only, but we take a write lock so that timing
            // info is more accurate.
            Client::WriteContext ctx( txn, nsString );

            Status prepInLockStatus = deleteExecutor.prepareInLock( ctx.ctx().db() );
            if ( !prepInLockStatus.isOK()) {
                return prepInLockStatus;
            }

            PlanExecutor* exec = deleteExecutor.getPlanExecutor();
            const ScopedExecutorRegistration safety( exec );

            // Explain the plan tree.
            return Explain::explainStages( txn, exec, verbosity, out );
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
