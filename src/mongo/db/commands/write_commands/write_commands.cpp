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
#include "mongo/db/commands/write_commands/batch_executor.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/insert.h"
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

    // This is set in rs.cpp when the replica set is initialized, and is stored in dbcommands.cpp
    // for now.  See dbcommands.cpp
    extern BSONObj* getLastErrorDefault;

    WriteCmd::WriteCmd( const StringData& name, BatchedCommandRequest::BatchType writeType ) :
        Command( name ), _writeType( writeType ) {
    }

    // Write commands are fanned out in oplog as single writes.
    bool WriteCmd::logTheOp() { return false; }

    // Slaves can't perform writes.
    bool WriteCmd::slaveOk() const { return false; }

    // Write commands acquire write lock, but not for entire length of execution.
    Command::LockType WriteCmd::locktype() const { return NONE; }

    Status WriteCmd::checkAuthForCommand( ClientBasic* client,
                                          const std::string& dbname,
                                          const BSONObj& cmdObj ) {

        Status status( auth::checkAuthForWriteCommand( client->getAuthorizationSession(),
                _writeType,
                NamespaceString( parseNs( dbname, cmdObj ) ),
                cmdObj ));

        if ( !status.isOK() ) {
            setLastError( status.code(), status.reason().c_str() );
        }

        return status;
    }

    // Write commands are counted towards their corresponding opcounters, not command opcounters.
    bool WriteCmd::shouldAffectCommandCounter() const { return false; }

    bool WriteCmd::run(const string& dbName,
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

            // Batch parse failure
            response.setOk( false );
            response.setN( 0 );
            response.setErrCode( ErrorCodes::FailedToParse );
            response.setErrMessage( errMsg );

            dassert( response.isValid( &errMsg ) );
            result.appendElements( response.toBSON() );

            // TODO
            // There's a pending issue about how to report response here. If we use
            // the command infra-structure, we should reuse the 'errmsg' field. But
            // we have already filed that message inside the BatchCommandResponse.
            // return response.getOk();
            return true;
        }

        // Note that this is a runCommmand, and therefore, the database and the collection name
        // are in different parts of the grammar for the command. But it's more convenient to
        // work with a NamespaceString. We built it here and replace it in the parsed command.
        // Internally, everything work with the namespace string as opposed to just the
        // collection name.
        NamespaceString nss(dbName, request.getNS());
        request.setNS(nss.ns());

        Status status = userAllowedWriteNS( nss );
        if ( !status.isOK() )
            return appendCommandStatus( result, status );

        if ( cc().curop() )
            cc().curop()->setNS( nss.ns() );

        if ( request.getBatchType() == BatchedCommandRequest::BatchType_Insert ) {
            // check all docs
            BatchedInsertRequest* insertRequest = request.getInsertRequest();
            vector<BSONObj>& docsToInsert = insertRequest->getDocuments();
            for ( size_t i = 0; i < docsToInsert.size(); i++ ) {
                StatusWith<BSONObj> fixed = fixDocumentForInsert( docsToInsert[i] );
                if ( !fixed.isOK() ) {
                    return appendCommandStatus( result, fixed.getStatus() );
                }
                if ( fixed.getValue().isEmpty() )
                    continue;
                docsToInsert[i] = fixed.getValue();
            }
        }

        BSONObj defaultWriteConcern;
        // This is really bad - it's only safe because we leak the defaults by overriding them with
        // new defaults and because we never reset to an empty default.
        // TODO: fix this for sane behavior where we query repl set object
        if ( getLastErrorDefault ) defaultWriteConcern = *getLastErrorDefault;
        if ( defaultWriteConcern.isEmpty() ) {
            BSONObjBuilder b;
            b.append( "w", 1 );
            defaultWriteConcern = b.obj();
        }

        WriteBatchExecutor writeBatchExecutor(defaultWriteConcern,
                                              &cc(),
                                              &globalOpCounters,
                                              lastError.get());

        writeBatchExecutor.executeBatch( request, &response );

        result.appendElements( response.toBSON() );

        // TODO
        // There's a pending issue about how to report response here. If we use
        // the command infra-structure, we should reuse the 'errmsg' field. But
        // we have already filed that message inside the BatchCommandResponse.
        // return response.getOk();
        return true;
    }

    CmdInsert::CmdInsert() :
        WriteCmd( "insert", BatchedCommandRequest::BatchType_Insert ) {
    }

    void CmdInsert::help( stringstream& help ) const {
        help << "insert documents";
    }

    CmdUpdate::CmdUpdate() :
        WriteCmd( "update", BatchedCommandRequest::BatchType_Update ) {
    }

    void CmdUpdate::help( stringstream& help ) const {
        help << "update documents";
    }

    CmdDelete::CmdDelete() :
        WriteCmd( "delete", BatchedCommandRequest::BatchType_Delete ) {
    }

    void CmdDelete::help( stringstream& help ) const {
        help << "delete documents";
    }

} // namespace mongo
