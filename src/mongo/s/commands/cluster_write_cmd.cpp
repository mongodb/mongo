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

#include "mongo/base/init.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/s/batched_command_request.h"
#include "mongo/s/batched_command_response.h"
#include "mongo/s/batch_write_exec.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/dbclient_multi_command.h"

namespace mongo {

    /**
     * Base class for mongos write commands.  Cluster write commands support batch writes and write
     * concern, and return per-item error information.  All cluster write commands use the entry
     * point ClusterWriteCmd::run().
     *
     * Batch execution (targeting and dispatching) is performed by the BatchWriteExec class.
     */
    class ClusterWriteCmd : public Command {
    MONGO_DISALLOW_COPYING(ClusterWriteCmd);
    public:

        virtual ~ClusterWriteCmd() {
        }

        bool logTheOp() {
            return false;
        }

        bool slaveOk() const {
            return false;
        }

        LockType locktype() const {
            return Command::NONE;
        }

        void addRequiredPrivileges( const std::string& dbname,
                                    const BSONObj& cmdObj,
                                    std::vector<Privilege>* out ) {
            ActionSet actions;
            actions.addAction( _action );
            out->push_back( Privilege( parseResourcePattern( dbname, cmdObj ), actions ) );
        }

        // Cluster write command entry point.
        bool run( const string& dbname,
                  BSONObj& cmdObj,
                  int options,
                  string& errmsg,
                  BSONObjBuilder& result,
                  bool fromRepl );

    protected:

        /**
         * Instantiates a command that can be invoked by "name", which will be capable of issuing
         * write batches of type "writeType", and will require privilege "action" to run.
         */
        ClusterWriteCmd( const StringData& name,
                         BatchedCommandRequest::BatchType writeType,
                         ActionType action ) :
            Command( name ), _action( action ), _writeType( writeType ) {
        }

    private:

        // Privilege required to execute command.
        ActionType _action;

        // Type of batch (e.g. insert).
        BatchedCommandRequest::BatchType _writeType;
    };

    class ClusterCmdInsert : public ClusterWriteCmd {
    MONGO_DISALLOW_COPYING(ClusterCmdInsert);
    public:
        ClusterCmdInsert() :
            ClusterWriteCmd( "insert",
                             BatchedCommandRequest::BatchType_Insert,
                             ActionType::insert ) {
        }

        void help( stringstream& help ) const {
            help << "insert documents";
        }
    };

    class ClusterCmdUpdate : public ClusterWriteCmd {
    MONGO_DISALLOW_COPYING(ClusterCmdUpdate);
    public:
        ClusterCmdUpdate() :
            ClusterWriteCmd( "update",
                             BatchedCommandRequest::BatchType_Update,
                             ActionType::update ) {
        }

        void help( stringstream& help ) const {
            help << "update documents";
        }
    };

    class ClusterCmdDelete : public ClusterWriteCmd {
    MONGO_DISALLOW_COPYING(ClusterCmdDelete);
    public:
        ClusterCmdDelete() :
            ClusterWriteCmd( "delete",
                             BatchedCommandRequest::BatchType_Delete,
                             ActionType::remove ) {
        }

        void help( stringstream& help ) const {
            help << "delete documents";
        }
    };

    //
    // Cluster write command implementation(s) below
    //

    bool ClusterWriteCmd::run( const string& dbName,
                               BSONObj& cmdObj,
                               int options,
                               string& errMsg,
                               BSONObjBuilder& result,
                               bool ) {

        BatchedCommandRequest request( _writeType );
        BatchedCommandResponse response;

        // TODO: if we do namespace parsing, push this to the type
        if ( !request.parseBSON( cmdObj, &errMsg ) || !request.isValid( &errMsg ) ) {
            // Batch parse failure
            response.setOk( false );
            response.setErrCode( ErrorCodes::FailedToParse );
            response.setErrMessage( errMsg );
            result.appendElements( response.toBSON() );

            // TODO
            // There's a pending issue about how to report response here. If we use
            // the command infra-structure, we should reuse the 'errmsg' field. But
            // we have already filed that message inside the BatchCommandResponse.
            // return response.getOk();
            return true;
        }

        //
        // Assemble the batch executor and run the batch
        //

        ChunkManagerTargeter targeter;

        NamespaceString nss( dbName, request.getNS() );
        request.setNS( nss.ns() );

        Status targetInitStatus = targeter.init( NamespaceString( request.getNS() ) );

        if ( !targetInitStatus.isOK() ) {

            warning() << "could not initialize targeter for write op in collection "
                      << request.getNS() << endl;

            // Errors will be reported in response if we are unable to target
        }

        DBClientMultiCommand dispatcher;

        BatchWriteExec exec( &targeter, &dispatcher );

        exec.executeBatch( request, &response );

        result.appendElements( response.toBSON() );

        // TODO
        // There's a pending issue about how to report response here. If we use
        // the command infra-structure, we should reuse the 'errmsg' field. But
        // we have already filed that message inside the BatchCommandResponse.
        // return response.getOk();
        return true;
    }

    //
    // Register write commands at startup
    //

    namespace {

        MONGO_INITIALIZER(RegisterWriteCommands)(InitializerContext* context) {
            // Leaked intentionally: a Command registers itself when constructed.
            new ClusterCmdInsert();
            new ClusterCmdUpdate();
            new ClusterCmdDelete();
            return Status::OK();
        }

    } // namespace

} // namespace mongo
