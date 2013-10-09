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

#include "mongo/s/batch_write_exec.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/s/batched_command_request.h"
#include "mongo/s/batched_command_response.h"
#include "mongo/s/mock_multi_command.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;

    //
    // Tests for the BatchWriteExec
    //

    TEST(BatchWriteExecTests, SingleOp) {

        //
        // Basic execution test
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED(), ConnectionString() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpoint,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << MAXKEY ) ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Insert );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Do single-target, single doc batch write op

        request.getInsertRequest()->addToDocuments( BSON( "x" << 1 ) );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        MockMultiCommand dispatcher;

        BatchWriteExec exec( &targeter, &dispatcher );

        BatchedCommandResponse response;
        exec.executeBatch( request, &response );

        ASSERT( response.getOk() );
    }

    TEST(BatchWriteExecTests, SingleOpError) {

        //
        // Basic error test
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED(), ConnectionString() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpoint,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << MAXKEY ) ) );

        vector<MockEndpoint*> mockEndpoints;
        BatchedErrorDetail error;
        error.setErrCode( ErrorCodes::UnknownError );
        error.setErrMessage( "mock error" );
        mockEndpoints.push_back( new MockEndpoint( endpoint.shardHost, error ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Insert );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Do single-target, single doc batch write op

        request.getInsertRequest()->addToDocuments( BSON( "x" << 1 ) );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        MockMultiCommand dispatcher;
        dispatcher.init( mockEndpoints );

        BatchWriteExec exec( &targeter, &dispatcher );

        BatchedCommandResponse response;
        exec.executeBatch( request, &response );

        ASSERT( !response.getOk() );
        ASSERT_EQUALS( response.getErrCode(), error.getErrCode() );
        ASSERT_EQUALS( response.getErrMessage(), error.getErrMessage() );
    }

    //
    // Test retryable errors
    //

    TEST(BatchWriteExecTests, RetryOpError) {

        //
        // Retry op in exec b/c of stale config
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED(), ConnectionString() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpoint,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << MAXKEY ) ) );

        vector<MockEndpoint*> mockEndpoints;
        BatchedErrorDetail error;
        error.setErrCode( ErrorCodes::StaleShardVersion );
        error.setErrInfo( BSONObj() ); // Needed for correct handling
        error.setErrMessage( "mock stale error" );
        mockEndpoints.push_back( new MockEndpoint( endpoint.shardHost, error ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Insert );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Do single-target, single doc batch write op

        request.getInsertRequest()->addToDocuments( BSON( "x" << 1 ) );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        MockMultiCommand dispatcher;
        dispatcher.init( mockEndpoints );

        BatchWriteExec exec( &targeter, &dispatcher );

        BatchedCommandResponse response;
        exec.executeBatch( request, &response );

        ASSERT( response.getOk() );
    }

} // unnamed namespace
