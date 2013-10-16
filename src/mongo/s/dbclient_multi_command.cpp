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
#include "mongo/s/shard.h"
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

                Message toSend;

                // see query.h for the protocol we are using here.
                BufBuilder bufB;
                bufB.appendNum( 0 ); // command/query options
                bufB.appendStr( command->dbName + ".$cmd" ); // write command ns
                bufB.appendNum( 0 ); // ntoskip (0 for command)
                bufB.appendNum( 1 ); // ntoreturn (1 for command)
                command->cmdObj.appendSelfToBufBuilder( bufB );
                toSend.setData( dbQuery, bufB.buf(), bufB.len() );

                // Send our command
                command->conn->say( toSend );
            }
            catch ( const DBException& ex ) {
                command->status = ex.toStatus();
                if ( NULL != command->conn ) delete command->conn;
                command->conn = NULL;
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

            Message toRecv;
            command->conn->recv( toRecv );

            // RPC was sent as a command, so a query result is what we get back
            QueryResult* recvdQuery = reinterpret_cast<QueryResult*>( toRecv.singleData() );
            BSONObj result( recvdQuery->data() );

            shardConnectionPool.release( command->endpoint.toString(), command->conn );
            command->conn = NULL;

            string errMsg;
            if ( !response->parseBSON( result, &errMsg ) || !response->isValid( &errMsg ) ) {
                return Status( ErrorCodes::FailedToParse, errMsg );
            }
        }
        catch ( const DBException& ex ) {

            delete command->conn;
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
