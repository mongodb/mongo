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
#include "mongo/db/client.h"
#include "mongo/db/commands/write_commands/batch_executor.h"
#include "mongo/db/json.h"
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

    WriteCmd::WriteCmd( const StringData& name,
                        BatchedCommandRequest::BatchType writeType,
                        ActionType action ) :
            Command( name ), _action( action ), _writeType( writeType ) {
    }

    // Write commands are fanned out in oplog as single writes.
    bool WriteCmd::logTheOp() { return false; }

    // Slaves can't perform writes.
    bool WriteCmd::slaveOk() const { return false; }

    // Write commands acquire write lock, but not for entire length of execution.
    Command::LockType WriteCmd::locktype() const { return NONE; }

    void WriteCmd::addRequiredPrivileges(const std::string& dbname,
                                         const BSONObj& cmdObj,
                                         std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(_action);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
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

        // TODO: if we do namespace parsing, push this to the type
        if ( !request.parseBSON( cmdObj, &errMsg ) || !request.isValid( &errMsg ) ) {
            // Batch parse failure
            response.setOk( false );
            response.setErrCode( 99999 );
            response.setErrMessage( errMsg );
            result.appendElements( response.toBSON() );
            return response.getOk();
        }

        {
            // Commands with locktype == NONE need to acquire a Context in order to set
            // CurOp::_ns.  Setting a CurOp's namespace is necessary for higher-level
            // functionality (e.g. profiling) to operate on the correct database (note that
            // WriteBatchExecutor doesn't do this for us, since its job is to create child CurOp
            // objects and operate on them).
            //
            // Acquire ReadContext momentarily, for satisfying this purpose.
            Client::ReadContext ctx( dbName + ".$cmd" );
        }

        WriteBatchExecutor writeBatchExecutor(&cc(), &globalOpCounters, lastError.get());

        writeBatchExecutor.executeBatch( request, &response );

        result.appendElements( response.toBSON() );
        return response.getOk();
    }

    CmdInsert::CmdInsert() :
            WriteCmd( "insert", BatchedCommandRequest::BatchType_Insert, ActionType::insert ) {
    }

    void CmdInsert::help( stringstream& help ) const {
        help << "insert documents";
    }

    CmdUpdate::CmdUpdate() :
            WriteCmd( "update", BatchedCommandRequest::BatchType_Update, ActionType::update ) {
    }

    void CmdUpdate::help( stringstream& help ) const {
        help << "update documents";
    }

    CmdDelete::CmdDelete() :
            WriteCmd( "delete", BatchedCommandRequest::BatchType_Delete, ActionType::remove ) {
    }

    void CmdDelete::help( stringstream& help ) const {
        help << "delete documents";
    }

} // namespace mongo
