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

#include "mongo/s/dbclient_multi_command.h"

#include "mongo/db/dbmessage.h"
#include "mongo/db/wire_version.h"
#include "mongo/s/shard.h"
#include "mongo/s/write_ops/batch_downconvert.h"
#include "mongo/s/write_ops/dbclient_safe_writer.h"
#include "mongo/util/net/message.h"

namespace mongo {

    DBClientMultiCommand::PendingCommand::PendingCommand( const ConnectionString& endpoint,
                                                          const StringData& dbName,
                                                          const BSONObj& cmdObj ) :
        endpoint( endpoint ),
        dbName( dbName.toString() ),
        cmdObj( cmdObj ),
        conn( NULL ),
        status( Status::OK() ) {
    }

    void DBClientMultiCommand::addCommand( const ConnectionString& endpoint,
                                           const StringData& dbName,
                                           const BSONSerializable& request ) {
        PendingCommand* command = new PendingCommand( endpoint, dbName, request.toBSON() );
        _pendingCommands.push_back( command );
    }

    namespace {

        //
        // Stuff we need for batch downconversion
        // TODO: Remove post-2.6
        //

        BatchedCommandRequest::BatchType getBatchWriteType( const BSONObj& cmdObj ) {
            string cmdName = cmdObj.firstElement().fieldName();
            if ( cmdName == "insert" ) return BatchedCommandRequest::BatchType_Insert;
            if ( cmdName == "update" ) return BatchedCommandRequest::BatchType_Update;
            if ( cmdName == "delete" ) return BatchedCommandRequest::BatchType_Delete;
            return BatchedCommandRequest::BatchType_Unknown;
        }

        bool isBatchWriteCommand( const BSONObj& cmdObj ) {
            return getBatchWriteType( cmdObj ) != BatchedCommandRequest::BatchType_Unknown;
        }

        bool hasBatchWriteFeature( DBClientBase* conn ) {
            return conn->getMinWireVersion() <= BATCH_COMMANDS
                   && conn->getMaxWireVersion() >= BATCH_COMMANDS;
        }

        /**
         * Parses and re-BSON's a batch write command in order to send it as a set of safe writes.
         */
        void legacySafeWrite( DBClientBase* conn,
                              const StringData& dbName,
                              const BSONObj& cmdRequest,
                              BSONObj* cmdResponse ) {

            // Translate from BSON
            BatchedCommandRequest request( getBatchWriteType( cmdRequest ) );

            // This should *always* parse correctly
            bool parsed = request.parseBSON( cmdRequest, NULL );
            (void) parsed; // for non-debug compile
            dassert( parsed && request.isValid( NULL ) );

            // Collection name is sent without db to the dispatcher
            request.setNS( dbName.toString() + "." + request.getNS() );

            DBClientSafeWriter safeWriter;
            BatchSafeWriter batchSafeWriter( &safeWriter );
            BatchedCommandResponse response;
            batchSafeWriter.safeWriteBatch( conn, request, &response );

            // Back to BSON
            dassert( response.isValid( NULL ) );
            *cmdResponse = response.toBSON();
        }
    }

    // THROWS
    static void sayAsCmd( DBClientBase* conn, const StringData& dbName, const BSONObj& cmdObj ) {
        Message toSend;

        // see query.h for the protocol we are using here.
        BufBuilder bufB;
        bufB.appendNum( 0 ); // command/query options
        bufB.appendStr( dbName.toString() + ".$cmd" ); // write command ns
        bufB.appendNum( 0 ); // ntoskip (0 for command)
        bufB.appendNum( 1 ); // ntoreturn (1 for command)
        cmdObj.appendSelfToBufBuilder( bufB );
        toSend.setData( dbQuery, bufB.buf(), bufB.len() );

        // Send our command
        conn->say( toSend );
    }

    // THROWS
    static void recvAsCmd( DBClientBase* conn, Message* toRecv, BSONObj* result ) {

        if ( !conn->recv( *toRecv ) ) {
            // Confusingly, socket exceptions here are written to the log, not thrown.
            uasserted( 17255, "error receiving write command response, "
                       "possible socket exception - see logs" );
        }

        // A query result is returned from commands
        QueryResult* recvdQuery = reinterpret_cast<QueryResult*>( toRecv->singleData() );
        *result = BSONObj( recvdQuery->data() );
    }

    void DBClientMultiCommand::sendAll() {

        for ( deque<PendingCommand*>::iterator it = _pendingCommands.begin();
            it != _pendingCommands.end(); ++it ) {

            PendingCommand* command = *it;
            dassert( NULL == command->conn );

            try {
                // TODO: Figure out how to handle repl sets, configs
                dassert( command->endpoint.type() == ConnectionString::MASTER ||
                    command->endpoint.type() == ConnectionString::CUSTOM );
                command->conn = shardConnectionPool.get( command->endpoint, 0 /*timeout*/);

                if ( hasBatchWriteFeature( command->conn )
                     || !isBatchWriteCommand( command->cmdObj ) ) {
                    // Do normal command dispatch
                    sayAsCmd( command->conn, command->dbName, command->cmdObj );
                }
                else {
                    // Sending a batch as safe writes necessarily blocks, so we can't do anything
                    // here.  Instead we do the safe writes in recvAny(), which can block.
                }
            }
            catch ( const DBException& ex ) {
                command->status = ex.toStatus();

                if ( NULL != command->conn ) {

                    // Confusingly, the pool needs to know about failed connections so that it can
                    // invalidate other connections which might be bad.  But if the connection
                    // doesn't seem bad, don't send it back, because we don't want to reuse it.
                    if ( !command->conn->isFailed() ) {
                        delete command->conn;
                    }
                    else {
                        shardConnectionPool.release( command->endpoint.toString(), command->conn );
                    }

                    command->conn = NULL;
                }
            }
        }
    }

    int DBClientMultiCommand::numPending() const {
        return static_cast<int>( _pendingCommands.size() );
    }

    Status DBClientMultiCommand::recvAny( ConnectionString* endpoint, BSONSerializable* response ) {

        scoped_ptr<PendingCommand> command( _pendingCommands.front() );
        _pendingCommands.pop_front();

        *endpoint = command->endpoint;
        if ( !command->status.isOK() ) return command->status;

        dassert( NULL != command->conn );

        try {

            // Holds the data and BSONObj for the command result
            Message toRecv;
            BSONObj result;

            if ( hasBatchWriteFeature( command->conn )
                 || !isBatchWriteCommand( command->cmdObj ) ) {
                // Recv data from command sent earlier
                recvAsCmd( command->conn, &toRecv, &result );
            }
            else {
                // We can safely block in recvAny, so dispatch writes as safe writes for hosts
                // that don't understand batch write commands.
                legacySafeWrite( command->conn, command->dbName, command->cmdObj, &result );
            }

            shardConnectionPool.release( command->endpoint.toString(), command->conn );
            command->conn = NULL;

            string errMsg;
            if ( !response->parseBSON( result, &errMsg ) || !response->isValid( &errMsg ) ) {
                return Status( ErrorCodes::FailedToParse, errMsg );
            }
        }
        catch ( const DBException& ex ) {

            // Confusingly, the pool needs to know about failed connections so that it can
            // invalidate other connections which might be bad.  But if the connection doesn't seem
            // bad, don't send it back, because we don't want to reuse it.
            if ( !command->conn->isFailed() ) {
                delete command->conn;
            }
            else {
                shardConnectionPool.release( command->endpoint.toString(), command->conn );
            }
            command->conn = NULL;

            return ex.toStatus();
        }

        return Status::OK();
    }

    DBClientMultiCommand::~DBClientMultiCommand() {

        // Cleanup anything outstanding, do *not* return stuff to the pool, that might error
        for ( deque<PendingCommand*>::iterator it = _pendingCommands.begin();
            it != _pendingCommands.end(); ++it ) {

            PendingCommand* command = *it;

            if ( NULL != command->conn ) delete command->conn;
            delete command;
            command = NULL;
        }

        _pendingCommands.clear();
    }
}
