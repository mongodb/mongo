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

        // TODO: Remove this when we standardize GLE reporting from commands
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
            return appendCommandStatus( result, Status( ErrorCodes::FailedToParse, errMsg ) );
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
        return response.getOk();
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
