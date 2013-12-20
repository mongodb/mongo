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

#include "mongo/s/write_ops/batch_write_op.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_delete_document.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;

    TEST(WriteOpTests, TargetSingleOp) {

        //
        // Single-op targeting test
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED() );

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

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();
        Status status = batchOp.targetBatch( targeter, false, &targeted );

        ASSERT( status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 1u );
        assertEndpointsEqual( targeted.front()->getEndpoint(), endpoint );

        BatchedCommandResponse response;
        response.setOk( true );
        response.setN( 0 );
        ASSERT( response.isValid( NULL ) );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( batchOp.isFinished() );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );
        ASSERT( clientResponse.getOk() );
    }

    WriteErrorDetail* buildError( int code, const BSONObj& info, const string& message ) {

        WriteErrorDetail* error = new WriteErrorDetail();
        error->setErrCode( code );
        error->setErrInfo( info );
        error->setErrMessage( message );

        return error;
    }

    void setBatchError( const WriteErrorDetail& error, BatchedCommandResponse* response ) {
        response->setOk( false );
        response->setN( 0 );
        response->setErrCode( error.getErrCode() );
        response->setErrMessage( error.getErrMessage() );
        ASSERT( response->isValid( NULL ) );
    }

    TEST(WriteOpTests, TargetSingleError) {

        //
        // Single-op error test
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED() );

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

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();
        Status status = batchOp.targetBatch( targeter, false, &targeted );

        ASSERT( status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 1u );
        assertEndpointsEqual( targeted.front()->getEndpoint(), endpoint );

        scoped_ptr<WriteErrorDetail> error( buildError( ErrorCodes::UnknownError,
                                                          BSON( "data" << 12345 ),
                                                          "message" ) );
        BatchedCommandResponse response;
        setBatchError( *error, &response );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( batchOp.isFinished() );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );

        ASSERT( !clientResponse.getOk() );
        ASSERT_EQUALS( clientResponse.getErrCode(), error->getErrCode() );
        ASSERT( clientResponse.getErrMessage().find( error->getErrMessage()) != string::npos );
    }

    TEST(WriteOpTests, TargetMultiOpSameShard) {

        //
        // Multi-op targeting test
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpoint,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << MAXKEY ) ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Insert );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Do single-target, multi-doc batch write op

        request.getInsertRequest()->addToDocuments( BSON( "x" << 1 ) );
        request.getInsertRequest()->addToDocuments( BSON( "x" << 2 ) );

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();
        Status status = batchOp.targetBatch( targeter, false, &targeted );

        ASSERT( status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 1u );
        ASSERT_EQUALS( targeted.front()->getWrites().size(), 2u );
        assertEndpointsEqual( targeted.front()->getEndpoint(), endpoint );

        BatchedCommandResponse response;
        response.setOk( true );
        response.setN( 0 );
        ASSERT( response.isValid( NULL ) );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( batchOp.isFinished() );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );
        ASSERT( clientResponse.getOk() );
    }

    struct EndpointComp {
        bool operator()( const TargetedWriteBatch* writeA,
                         const TargetedWriteBatch* writeB ) const {
            return writeA->getEndpoint().shardName.compare( writeB->getEndpoint().shardName ) < 0;
        }
    };

    inline void sortByEndpoint( vector<TargetedWriteBatch*>* writes ) {
        std::sort( writes->begin(), writes->end(), EndpointComp() );
    }

    TEST(WriteOpTests, TargetMultiOpTwoShards) {

        //
        // Multi-op targeting test
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpointA( "shardA", ChunkVersion::IGNORED() );
        ShardEndpoint endpointB( "shardB", ChunkVersion::IGNORED() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpointA,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << 0 ) ) );
        mockRanges.push_back( new MockRange( endpointB,
                                             nss,
                                             BSON( "x" << 0 ),
                                             BSON( "x" << MAXKEY ) ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Insert );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Do multi-target, multi-doc batch write op

        request.getInsertRequest()->addToDocuments( BSON( "x" << -1 ) );
        request.getInsertRequest()->addToDocuments( BSON( "x" << 1 ) );

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();
        Status status = batchOp.targetBatch( targeter, false, &targeted );

        ASSERT( status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 2u );
        sortByEndpoint( &targeted );
        ASSERT_EQUALS( targeted.front()->getWrites().size(), 1u );
        assertEndpointsEqual( targeted.front()->getEndpoint(), endpointA );
        ASSERT_EQUALS( targeted.back()->getWrites().size(), 1u );
        assertEndpointsEqual( targeted.back()->getEndpoint(), endpointB );

        BatchedCommandResponse response;
        response.setOk( true );
        response.setN( 0 );
        ASSERT( response.isValid( NULL ) );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( !batchOp.isFinished() );
        batchOp.noteBatchResponse( *targeted.back(), response, NULL );
        ASSERT( batchOp.isFinished() );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );
        ASSERT( clientResponse.getOk() );
    }

    BatchedDeleteDocument* buildDeleteDoc( const BSONObj& doc ) {

        BatchedDeleteDocument* deleteDoc = new BatchedDeleteDocument();

        string errMsg;
        bool ok = deleteDoc->parseBSON( doc, &errMsg );
        ASSERT_EQUALS( errMsg, "" );
        ASSERT( ok );
        return deleteDoc;
    }

    TEST(WriteOpTests, TargetMultiOpTwoShardsEach) {

        //
        // Multi-op targeting test where each op goes to both shards
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpointA( "shardA", ChunkVersion::IGNORED() );
        ShardEndpoint endpointB( "shardB", ChunkVersion::IGNORED() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpointA,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << 0 ) ) );
        mockRanges.push_back( new MockRange( endpointB,
                                             nss,
                                             BSON( "x" << 0 ),
                                             BSON( "x" << MAXKEY ) ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Delete );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Each op goes to both shards

        BSONObj queryA = BSON( "x" << GTE << -1 << LT << 2 );
        request.getDeleteRequest()->addToDeletes( buildDeleteDoc( BSON( "q" << queryA ) ) );
        BSONObj queryB = BSON( "x" << GTE << -2 << LT << 1 );
        request.getDeleteRequest()->addToDeletes( buildDeleteDoc( BSON( "q" << queryB ) ) );

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();
        Status status = batchOp.targetBatch( targeter, false, &targeted );

        ASSERT( status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 2u );
        sortByEndpoint( &targeted );
        assertEndpointsEqual( targeted.front()->getEndpoint(), endpointA );
        assertEndpointsEqual( targeted.back()->getEndpoint(), endpointB );
        ASSERT_EQUALS( targeted.front()->getWrites().size(), 2u );
        ASSERT_EQUALS( targeted.back()->getWrites().size(), 2u );

        BatchedCommandResponse response;
        response.setOk( true );
        response.setN( 0 );
        ASSERT( response.isValid( NULL ) );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( !batchOp.isFinished() );
        batchOp.noteBatchResponse( *targeted.back(), response, NULL );
        ASSERT( batchOp.isFinished() );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );
        ASSERT( clientResponse.getOk() );
    }

    TEST(WriteOpTests, TargetMultiOpTwoShardsEachError) {

        //
        // Multi-op targeting test where each op goes to both shards and there's an error on one
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpointA( "shardA", ChunkVersion::IGNORED() );
        ShardEndpoint endpointB( "shardB", ChunkVersion::IGNORED() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpointA,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << 0 ) ) );
        mockRanges.push_back( new MockRange( endpointB,
                                             nss,
                                             BSON( "x" << 0 ),
                                             BSON( "x" << MAXKEY ) ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Delete );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Each op goes to both shards

        BSONObj queryA = BSON( "x" << GTE << -1 << LT << 2 );
        request.getDeleteRequest()->addToDeletes( buildDeleteDoc( BSON( "q" << queryA ) ) );
        BSONObj queryB = BSON( "x" << GTE << -2 << LT << 1 );
        request.getDeleteRequest()->addToDeletes( buildDeleteDoc( BSON( "q" << queryB ) ) );

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();
        Status status = batchOp.targetBatch( targeter, false, &targeted );

        ASSERT( status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 2u );
        sortByEndpoint( &targeted );
        assertEndpointsEqual( targeted.front()->getEndpoint(), endpointA );
        assertEndpointsEqual( targeted.back()->getEndpoint(), endpointB );
        ASSERT_EQUALS( targeted.front()->getWrites().size(), 2u );
        ASSERT_EQUALS( targeted.back()->getWrites().size(), 2u );

        // First shard write ok
        BatchedCommandResponse response;
        response.setOk( true );
        response.setN( 0 );
        ASSERT( response.isValid( NULL ) );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( !batchOp.isFinished() );

        // Second shard write fails
        BatchedCommandResponse errorResponse;
        scoped_ptr<WriteErrorDetail> error( buildError( ErrorCodes::UnknownError,
                                                          BSON( "data" << 12345 ),
                                                          "message" ) );
        setBatchError( *error, &errorResponse );
        batchOp.noteBatchResponse( *targeted.back(), errorResponse, NULL );
        ASSERT( batchOp.isFinished() );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );
        ASSERT( !clientResponse.getOk() );
        ASSERT_FALSE( clientResponse.isErrDetailsSet() );
    }

    TEST(WriteOpTests, TargetMultiOpFailedTarget) {

        //
        // Targeting failure on batch op
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpoint,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << 0 ) ) ); // positive vals untargetable

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Insert );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Do single-target, multi-doc batch write op

        request.getInsertRequest()->addToDocuments( BSON( "x" << 1 ) );
        request.getInsertRequest()->addToDocuments( BSON( "x" << 2 ) );

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();

        // Error on targeting failure
        Status status = batchOp.targetBatch( targeter, false, &targeted );
        ASSERT( !status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 0u );

        // Record targeting failures
        status = batchOp.targetBatch( targeter, true, &targeted );
        ASSERT( status.isOK() );
        ASSERT( batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 0u );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );
        ASSERT( clientResponse.getOk() );
        ASSERT_EQUALS( clientResponse.sizeErrDetails(), 2u );
    }

    TEST(WriteOpTests, TargetMultiOpTwoShardsEachWCError) {

        //
        // Multi-op targeting test where each op goes to both shards and both will return a
        // write concern error.
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpointA( "shardA", ChunkVersion::IGNORED() );
        ShardEndpoint endpointB( "shardB", ChunkVersion::IGNORED() );

        vector<MockRange*> mockRanges;
        mockRanges.push_back( new MockRange( endpointA,
                                             nss,
                                             BSON( "x" << MINKEY ),
                                             BSON( "x" << 0 ) ) );
        mockRanges.push_back( new MockRange( endpointB,
                                             nss,
                                             BSON( "x" << 0 ),
                                             BSON( "x" << MAXKEY ) ) );

        BatchedCommandRequest request( BatchedCommandRequest::BatchType_Delete );
        request.setNS( nss.ns() );
        request.setOrdered( false );
        request.setWriteConcern( BSONObj() );

        // Each op goes to both shards

        BSONObj queryA = BSON( "x" << GTE << -1 << LT << 2 );
        request.getDeleteRequest()->addToDeletes( buildDeleteDoc( BSON( "q" << queryA ) ) );
        BSONObj queryB = BSON( "x" << GTE << -2 << LT << 1 );
        request.getDeleteRequest()->addToDeletes( buildDeleteDoc( BSON( "q" << queryB ) ) );

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();
        Status status = batchOp.targetBatch( targeter, false, &targeted );

        ASSERT( status.isOK() );
        ASSERT( !batchOp.isFinished() );
        ASSERT_EQUALS( targeted.size(), 2u );
        sortByEndpoint( &targeted );
        assertEndpointsEqual( targeted.front()->getEndpoint(), endpointA );
        assertEndpointsEqual( targeted.back()->getEndpoint(), endpointB );
        ASSERT_EQUALS( targeted.front()->getWrites().size(), 2u );
        ASSERT_EQUALS( targeted.back()->getWrites().size(), 2u );

        // First shard write write concern fails.
        BatchedCommandResponse response;
        response.setOk( true );
        response.setN( 0 );

        WCErrorDetail firstShardWCError;
        firstShardWCError.setErrCode( ErrorCodes::UnknownError );
        string firstShardWCMsg( "s1 unknown" );
        firstShardWCError.setErrMessage( firstShardWCMsg );

        response.setWriteConcernError( firstShardWCError );

        ASSERT( response.isValid( NULL ) );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( !batchOp.isFinished() );

        // Second shard write write concern fails.
        BatchedCommandResponse response2;
        response2.setOk( true );
        response2.setN( 0 );

        WCErrorDetail secondShardWCError;
        secondShardWCError.setErrCode( ErrorCodes::UnknownError );
        string secondShardWCMsg( "s2 unknown" );
        secondShardWCError.setErrMessage( secondShardWCMsg );
        response2.setWriteConcernError( secondShardWCError );

        ASSERT( response2.isValid( NULL ) );

        batchOp.noteBatchResponse( *targeted.back(), response2, NULL );
        ASSERT( batchOp.isFinished() );

        BatchedCommandResponse clientResponse;
        batchOp.buildClientResponse( &clientResponse );
        ASSERT( clientResponse.getOk() );
        ASSERT_FALSE( clientResponse.isErrDetailsSet() );

        const WCErrorDetail* fullWCError = clientResponse.getWriteConcernError();
        ASSERT_EQUALS( ErrorCodes::WriteConcernFailed, fullWCError->getErrCode() );

        const string wcMessage( fullWCError->getErrMessage() );
        ASSERT( wcMessage.find( firstShardWCMsg ) != string::npos );
        ASSERT( wcMessage.find( secondShardWCMsg ) != string::npos );
    }

    //
    // Test retryable errors
    //

    TEST(WriteOpTests, SingleStaleError) {

        //
        // Single-op stale version test
        //

        NamespaceString nss( "foo.bar" );

        ShardEndpoint endpoint( "shard", ChunkVersion::IGNORED() );

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

        BatchWriteOp batchOp;
        batchOp.initClientRequest( &request );
        ASSERT( !batchOp.isFinished() );

        MockNSTargeter targeter;
        targeter.init( mockRanges );

        //
        // Target once
        //

        OwnedPointerVector<TargetedWriteBatch> targetedOwned;
        vector<TargetedWriteBatch*>& targeted = targetedOwned.mutableVector();

        ASSERT( batchOp.targetBatch( targeter, false, &targeted ).isOK() );
        ASSERT_EQUALS( targeted.size(), 1u );

        auto_ptr<WriteErrorDetail> error( buildError( ErrorCodes::StaleShardVersion,
                                                      BSONObj(),
                                                      "mock stale version" ) );
        error->setIndex( 0 );

        BatchedCommandResponse response;
        response.addToErrDetails( error.release() );
        response.setOk( 1 );

        batchOp.noteBatchResponse( *targeted.front(), response, NULL );
        ASSERT( !batchOp.isFinished() );

        //
        // Target again
        //

        OwnedPointerVector<TargetedWriteBatch> nextTargetedOwned;
        vector<TargetedWriteBatch*>& nextTargeted = nextTargetedOwned.mutableVector();

        ASSERT( batchOp.targetBatch( targeter, false, &nextTargeted ).isOK() );
        ASSERT_EQUALS( nextTargeted.size(), 1u );

        BatchedCommandResponse nextResponse;
        nextResponse.setOk( true );
        nextResponse.setN( 0 );
        ASSERT( nextResponse.isValid( NULL ) );

        batchOp.noteBatchResponse( *nextTargeted.front(), nextResponse, NULL );
        ASSERT( batchOp.isFinished() );
    }

} // unnamed namespace
