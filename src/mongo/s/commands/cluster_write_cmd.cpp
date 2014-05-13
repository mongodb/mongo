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
#include "mongo/db/client_basic.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/write_commands/write_commands_common.h"
#include "mongo/s/cluster_write.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/client_info.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batch_upconvert.h"
#include "mongo/db/stats/counters.h"

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

        bool slaveOk() const {
            return false;
        }

        bool isWriteCommandForConfigServer() const { return false; }

        Status checkAuthForCommand( ClientBasic* client,
                                    const std::string& dbname,
                                    const BSONObj& cmdObj ) {

            Status status = auth::checkAuthForWriteCommand( client->getAuthorizationSession(),
                                                            _writeType,
                                                            NamespaceString( parseNs( dbname,
                                                                                      cmdObj ) ),
                                                            cmdObj );

            // TODO: Remove this when we standardize GLE reporting from commands
            if ( !status.isOK() ) {
                setLastError( status.code(), status.reason().c_str() );
            }

            return status;
        }

        // Cluster write command entry point.
        bool run(TransactionExperiment* txn, const string& dbname,
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
        ClusterWriteCmd( const StringData& name, BatchedCommandRequest::BatchType writeType ) :
            Command( name ), _writeType( writeType ) {
        }

    private:

        // Type of batch (e.g. insert).
        BatchedCommandRequest::BatchType _writeType;
    };

    class ClusterCmdInsert : public ClusterWriteCmd {
    MONGO_DISALLOW_COPYING(ClusterCmdInsert);
    public:
        ClusterCmdInsert() :
            ClusterWriteCmd( "insert", BatchedCommandRequest::BatchType_Insert ) {
        }

        void help( stringstream& help ) const {
            help << "insert documents";
        }
    };

    class ClusterCmdUpdate : public ClusterWriteCmd {
    MONGO_DISALLOW_COPYING(ClusterCmdUpdate);
    public:
        ClusterCmdUpdate() :
            ClusterWriteCmd( "update", BatchedCommandRequest::BatchType_Update ) {
        }

        void help( stringstream& help ) const {
            help << "update documents";
        }
    };

    class ClusterCmdDelete : public ClusterWriteCmd {
    MONGO_DISALLOW_COPYING(ClusterCmdDelete);
    public:
        ClusterCmdDelete() :
            ClusterWriteCmd( "delete", BatchedCommandRequest::BatchType_Delete ) {
        }

        void help( stringstream& help ) const {
            help << "delete documents";
        }
    };

    //
    // Cluster write command implementation(s) below
    //

    bool ClusterWriteCmd::run(TransactionExperiment* txn, const string& dbName,
                               BSONObj& cmdObj,
                               int options,
                               string& errMsg,
                               BSONObjBuilder& result,
                               bool ) {

        BatchedCommandRequest request( _writeType );
        BatchedCommandResponse response;
        ClusterWriter writer( true /* autosplit */, 0 /* timeout */ );

        // NOTE: Sometimes this command is invoked with LE disabled for legacy writes
        LastError* cmdLastError = lastError.get( false );

        {
            // Disable the last error object for the duration of the write
            LastError::Disabled disableLastError( cmdLastError );

            // TODO: if we do namespace parsing, push this to the type
            if ( !request.parseBSON( cmdObj, &errMsg ) || !request.isValid( &errMsg ) ) {

                // Batch parse failure
                response.setOk( false );
                response.setErrCode( ErrorCodes::FailedToParse );
                response.setErrMessage( errMsg );
            }
            else {

                // Fixup the namespace to be a full ns internally
                NamespaceString nss( dbName, request.getNS() );
                request.setNS( nss.ns() );

                writer.write( request, &response );
            }

            dassert( response.isValid( NULL ) );
        }

        if ( cmdLastError ) {
            // Populate the lastError object based on the write response
            cmdLastError->reset();
            batchErrorToLastError( request, response, cmdLastError );
        }

        size_t numAttempts;
        if ( !response.getOk() ) {
            numAttempts = 0;
        } else if ( request.getOrdered() && response.isErrDetailsSet() ) {
            numAttempts = response.getErrDetailsAt(0)->getIndex() + 1; // Add one failed attempt
        } else {
            numAttempts = request.sizeWriteOps();
        }

        // TODO: increase opcounters by more than one
        if ( _writeType == BatchedCommandRequest::BatchType_Insert ) {
            for( size_t i = 0; i < numAttempts; ++i ) {
                globalOpCounters.gotInsert();
            }
        } else if ( _writeType == BatchedCommandRequest::BatchType_Update ) {
            for( size_t i = 0; i < numAttempts; ++i ) {
                globalOpCounters.gotUpdate();
            }
        } else if ( _writeType == BatchedCommandRequest::BatchType_Delete ) {
            for( size_t i = 0; i < numAttempts; ++i ) {
                globalOpCounters.gotDelete();
            }
        }

        // Save the last opTimes written on each shard for this client, to allow GLE to work
        if ( ClientInfo::exists() && writer.getStats().hasShardStats() ) {
            ClientInfo* clientInfo = ClientInfo::get( NULL );
            clientInfo->addHostOpTimes( writer.getStats().getShardStats().getWriteOpTimes() );
        }

        // TODO
        // There's a pending issue about how to report response here. If we use
        // the command infra-structure, we should reuse the 'errmsg' field. But
        // we have already filed that message inside the BatchCommandResponse.
        // return response.getOk();
        result.appendElements( response.toBSON() );
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
