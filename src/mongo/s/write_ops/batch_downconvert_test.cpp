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

#include "mongo/s/write_ops/batch_downconvert.h"

#include <deque>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/s/multi_command_dispatch.h"
#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;
    using std::vector;
    using std::deque;

    //
    // Tests for parsing GLE responses into write errors and write concern errors for write
    // commands.  These tests essentially document our expected 2.4 GLE behaviors.
    //

    TEST(GLEParsing, Empty) {

        const BSONObj gleResponse = fromjson( "{ok: 1.0, err: null}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( !errors.writeError.get() );
        ASSERT( !errors.wcError.get() );

        BatchSafeWriter::GLEStats stats;
        BatchSafeWriter::extractGLEStats( gleResponse, &stats );
        ASSERT_EQUALS( stats.n, 0 );
        ASSERT( stats.upsertedId.isEmpty() );
    }

    TEST(GLEParsing, WriteErr) {

        const BSONObj gleResponse = fromjson( "{ok: 1.0, err: 'message', code: 1000}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( errors.writeError.get() );
        ASSERT_EQUALS( errors.writeError->getErrMessage(), "message" );
        ASSERT_EQUALS( errors.writeError->getErrCode(), 1000 );
        ASSERT( !errors.wcError.get() );
    }

    TEST(GLEParsing, JournalFail) {

        const BSONObj gleResponse = fromjson( "{ok: 1.0, err: null, jnote: 'message'}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( !errors.writeError.get() );
        ASSERT( errors.wcError.get() );
        ASSERT_EQUALS( errors.wcError->getErrMessage(), "message" );
        ASSERT_EQUALS( errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed );
    }

    TEST(GLEParsing, ReplErr) {

        const BSONObj gleResponse = fromjson( "{ok: 1.0, err: 'norepl', wnote: 'message'}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( !errors.writeError.get() );
        ASSERT( errors.wcError.get() );
        ASSERT_EQUALS( errors.wcError->getErrMessage(), "message" );
        ASSERT_EQUALS( errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed );
    }

    TEST(GLEParsing, ReplTimeoutErr) {

        const BSONObj gleResponse =
            fromjson( "{ok: 1.0, err: 'timeout', errmsg: 'message', wtimeout: true}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( !errors.writeError.get() );
        ASSERT( errors.wcError.get() );
        ASSERT_EQUALS( errors.wcError->getErrMessage(), "message" );
        ASSERT( errors.wcError->getErrInfo()["wtimeout"].trueValue() );
        ASSERT_EQUALS( errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed );
    }

    TEST(GLEParsing, GLEFail) {

        const BSONObj gleResponse =
            fromjson( "{ok: 0.0, err: null, errmsg: 'message', code: 1000}" );

        BatchSafeWriter::GLEErrors errors;
        Status status = BatchSafeWriter::extractGLEErrors( gleResponse, &errors );
        ASSERT_NOT_OK( status );
        ASSERT_EQUALS( status.reason(), "message" );
        ASSERT_EQUALS( status.code(), 1000 );
    }

    TEST(GLEParsing, GLEFailNoCode) {

        const BSONObj gleResponse = fromjson( "{ok: 0.0, err: null, errmsg: 'message'}" );

        BatchSafeWriter::GLEErrors errors;
        Status status = BatchSafeWriter::extractGLEErrors( gleResponse, &errors );
        ASSERT_NOT_OK( status );
        ASSERT_EQUALS( status.reason(), "message" );
        ASSERT_EQUALS( status.code(), ErrorCodes::UnknownError );
    }

    TEST(GLEParsing, NotMasterGLEFail) {

        // Not master code in response
        const BSONObj gleResponse =
            fromjson( "{ok: 0.0, err: null, errmsg: 'message', code: 10990}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( !errors.writeError.get() );
        ASSERT( errors.wcError.get() );
        ASSERT_EQUALS( errors.wcError->getErrMessage(), "message" );
        ASSERT_EQUALS( errors.wcError->getErrCode(), 10990 );
    }

    TEST(GLEParsing, OldStaleWrite) {

        const BSONObj gleResponse =
            fromjson( "{ok: 1.0, err: null, writeback: 'abcde', writebackSince: 1}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( !errors.writeError.get() );
        ASSERT( !errors.wcError.get() );
    }

    TEST(GLEParsing, StaleWriteErrAndNotMasterGLEFail) {

        // Not master code in response
        const BSONObj gleResponse = fromjson( "{ok: 0.0, err: null, errmsg: 'message', code: 10990,"
                                              " writeback: 'abcde', writebackSince: 0}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( errors.writeError.get() );
        ASSERT_EQUALS( errors.writeError->getErrCode(), ErrorCodes::StaleShardVersion );
        ASSERT( errors.wcError.get() );
        ASSERT_EQUALS( errors.wcError->getErrMessage(), "message" );
        ASSERT_EQUALS( errors.wcError->getErrCode(), 10990 );
    }

    TEST(GLEParsing, WriteErrWithStats) {
        const BSONObj gleResponse = fromjson( "{ok: 1.0, n: 2, err: 'message', code: 1000}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( errors.writeError.get() );
        ASSERT_EQUALS( errors.writeError->getErrMessage(), "message" );
        ASSERT_EQUALS( errors.writeError->getErrCode(), 1000 );
        ASSERT( !errors.wcError.get() );

        BatchSafeWriter::GLEStats stats;
        BatchSafeWriter::extractGLEStats( gleResponse, &stats );
        ASSERT_EQUALS( stats.n, 2 );
        ASSERT( stats.upsertedId.isEmpty() );
    }

    TEST(GLEParsing, ReplTimeoutErrWithStats) {
        const BSONObj gleResponse =
            fromjson( "{ok: 1.0, err: 'timeout', errmsg: 'message', wtimeout: true,"
                      " n: 1, upserted: 'abcde'}" );

        BatchSafeWriter::GLEErrors errors;
        ASSERT_OK( BatchSafeWriter::extractGLEErrors( gleResponse, &errors ) );
        ASSERT( !errors.writeError.get() );
        ASSERT( errors.wcError.get() );
        ASSERT_EQUALS( errors.wcError->getErrMessage(), "message" );
        ASSERT( errors.wcError->getErrInfo()["wtimeout"].trueValue() );
        ASSERT_EQUALS( errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed );

        BatchSafeWriter::GLEStats stats;
        BatchSafeWriter::extractGLEStats( gleResponse, &stats );
        ASSERT_EQUALS( stats.n, 1 );
        ASSERT_EQUALS( stats.upsertedId.firstElement().str(), "abcde" );
    }

    //
    // Tests of the aggregation of 2.4 GLE responses into batch responses.
    //

    /**
     * Mock Safe Writer for testing
     */
    class MockSafeWriter : public SafeWriter {
    public:

        MockSafeWriter( const vector<BSONObj>& gleResponses ) :
            _gleResponses( gleResponses.begin(), gleResponses.end() ) {
        }

        virtual ~MockSafeWriter() {
        }

        Status safeWrite( DBClientBase* conn,
                          const BatchItemRef& batchItem,
                          const BSONObj& writeConcern,
                          BSONObj* gleResponse ) {
            BSONObj response = _gleResponses.front();
            _gleResponses.pop_front();
            *gleResponse = response;
            return Status::OK();
        }

        Status enforceWriteConcern( DBClientBase* conn,
                                    const StringData& dbName,
                                    const BSONObj& writeConcern,
                                    BSONObj* gleResponse ) {
            BSONObj response = _gleResponses.front();
            _gleResponses.pop_front();
            *gleResponse = response;
            return Status::OK();
        }

        deque<BSONObj> _gleResponses;
    };

    TEST(WriteBatchDownconvert, Basic) {

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null}" ) );

        MockSafeWriter mockWriter( gleResponses );
        BatchSafeWriter batchWriter( &mockWriter );

        BatchedCommandRequest cmdRequest( BatchedCommandRequest::BatchType_Insert );
        cmdRequest.setNS( "foo.bar" );
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
        request.addToDocuments(BSONObj());

        BatchedCommandResponse response;
        batchWriter.safeWriteBatch( NULL, cmdRequest, &response );

        ASSERT( response.getOk() );
        ASSERT_EQUALS( response.getN(), 1 );
        ASSERT( !response.isErrDetailsSet() );
        ASSERT( !response.isWriteConcernErrorSet() );
    }

    TEST(WriteBatchDownconvert, BasicUpsert) {

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null, n: 1, upserted : 'abcde'}" ) );

        MockSafeWriter mockWriter( gleResponses );
        BatchSafeWriter batchWriter( &mockWriter );

        BatchedCommandRequest cmdRequest( BatchedCommandRequest::BatchType_Update );
        cmdRequest.setNS( "foo.bar" );
        BatchedUpdateRequest& request = *cmdRequest.getUpdateRequest();
        request.addToUpdates(new BatchedUpdateDocument);

        BatchedCommandResponse response;
        batchWriter.safeWriteBatch( NULL, cmdRequest, &response );

        ASSERT( response.getOk() );
        ASSERT_EQUALS( response.getN(), 1 );
        ASSERT( response.isUpsertDetailsSet() );
        ASSERT_EQUALS( response.getUpsertDetailsAt(0)->getIndex(), 0 );
        ASSERT_EQUALS( response.getUpsertDetailsAt(0)->getUpsertedID().firstElement().str(),
                       "abcde" );
        ASSERT( !response.isErrDetailsSet() );
        ASSERT( !response.isWriteConcernErrorSet() );
    }

    TEST(WriteBatchDownconvert, WriteError) {

        // Error on first document, unordered

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 1.0, err: 'message', code: 12345}" ) );
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null}" ) );

        MockSafeWriter mockWriter( gleResponses );
        BatchSafeWriter batchWriter( &mockWriter );

        BatchedCommandRequest cmdRequest( BatchedCommandRequest::BatchType_Insert );
        cmdRequest.setNS( "foo.bar" );
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
        request.addToDocuments(BSONObj());
        request.addToDocuments(BSONObj());
        request.setOrdered(false);

        BatchedCommandResponse response;
        batchWriter.safeWriteBatch( NULL, cmdRequest, &response );

        ASSERT( response.getOk() );
        ASSERT_EQUALS( response.getN(), 1 );
        ASSERT_EQUALS( response.sizeErrDetails(), 1u );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getIndex(), 0);
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getErrMessage(), "message" );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getErrCode(), 12345 );
        ASSERT( !response.isWriteConcernErrorSet() );
    }

    TEST(WriteBatchDownconvert, WriteErrorAndReplError) {

        // Error on first document, write concern error on second document, unordered

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 1.0, err: 'message', code: 12345}" ) );
        gleResponses.push_back( fromjson( "{ok: 1.0, err: 'norepl', wnote: 'message'}" ) );

        MockSafeWriter mockWriter( gleResponses );
        BatchSafeWriter batchWriter( &mockWriter );

        BatchedCommandRequest cmdRequest( BatchedCommandRequest::BatchType_Insert );
        cmdRequest.setNS( "foo.bar" );
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
        request.addToDocuments( BSONObj() );
        request.addToDocuments( BSONObj() );
        request.setOrdered(false);

        BatchedCommandResponse response;
        batchWriter.safeWriteBatch( NULL, cmdRequest, &response );

        ASSERT( response.getOk() );
        ASSERT_EQUALS( response.getN(), 1 );
        ASSERT_EQUALS( response.sizeErrDetails(), 1u );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getIndex(), 0 );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getErrMessage(), "message" );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getErrCode(), 12345 );
        ASSERT( response.isWriteConcernErrorSet() );
        ASSERT_EQUALS( response.getWriteConcernError()->getErrMessage(), "message" );
        ASSERT_EQUALS( response.getWriteConcernError()->getErrCode(),
                       ErrorCodes::WriteConcernFailed );
    }

    TEST(WriteBatchDownconvert, FinalWriteErrorAndReplError) {

        // Error and write concern error on last document, need another gle to check write
        // concern if the last document had a write error in an unordered batch.

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null}" ) );
        gleResponses.push_back( fromjson( "{ok: 1.0, err: 'message', code: 12345}" ) );
        // Response is used *after* the last write for write concern error
        gleResponses.push_back( fromjson( "{ok: 1.0, err: 'norepl', wnote: 'message'}" ) );

        MockSafeWriter mockWriter( gleResponses );
        BatchSafeWriter batchWriter( &mockWriter );

        BatchedCommandRequest cmdRequest( BatchedCommandRequest::BatchType_Insert );
        cmdRequest.setNS( "foo.bar" );
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
        request.addToDocuments( BSONObj() );
        request.addToDocuments( BSONObj() );
        request.setOrdered(false);

        BatchedCommandResponse response;
        batchWriter.safeWriteBatch( NULL, cmdRequest, &response );

        ASSERT( response.getOk() );
        ASSERT_EQUALS( response.getN(), 1 );
        ASSERT_EQUALS( response.sizeErrDetails(), 1u );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getIndex(), 1 );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getErrMessage(), "message" );
        ASSERT_EQUALS( response.getErrDetailsAt(0)->getErrCode(), 12345 );
        ASSERT( response.isWriteConcernErrorSet() );
        ASSERT_EQUALS( response.getWriteConcernError()->getErrMessage(), "message" );
        ASSERT_EQUALS( response.getWriteConcernError()->getErrCode(),
                       ErrorCodes::WriteConcernFailed );
    }

    TEST(WriteBatchDownconvert, ReportOpTime) {

        // Checks that we correctly report the latest OpTime (needed to enforce backward-compatible
        // GLE calls after this call.

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null, lastOp: Timestamp(10, 0)}" ) );
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null, lastOp: Timestamp(20, 0)}" ) );

        MockSafeWriter mockWriter( gleResponses );
        BatchSafeWriter batchWriter( &mockWriter );

        BatchedCommandRequest cmdRequest( BatchedCommandRequest::BatchType_Insert );
        cmdRequest.setNS( "foo.bar" );
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
        request.addToDocuments(BSONObj());
        request.addToDocuments(BSONObj());

        BatchedCommandResponse response;
        batchWriter.safeWriteBatch( NULL, cmdRequest, &response );

        ASSERT( response.getOk() );
        ASSERT_EQUALS( response.getN(), 2 );
        ASSERT( !response.isErrDetailsSet() );
        ASSERT( !response.isWriteConcernErrorSet() );
        ASSERT_EQUALS( response.getLastOp().toStringPretty(), OpTime(20, 0).toStringPretty() );
    }

    //
    // Tests of processing and suppressing non-WC related fields from legacy GLE responses
    //

    TEST(LegacyGLESuppress, Basic) {

        const BSONObj gleResponse = fromjson( "{ok: 1.0, err: null}" );

        BSONObj stripped = BatchSafeWriter::stripNonWCInfo( gleResponse );
        ASSERT_EQUALS( stripped.nFields(), 2 ); // with err, ok : true
        ASSERT( stripped["ok"].trueValue() );
    }

    TEST(LegacyGLESuppress, BasicStats) {

        const BSONObj gleResponse =
            fromjson( "{ok: 0.0, err: 'message',"
                      " n: 1, nModified: 1, upserted: 'abc', updatedExisting: true}" );

        BSONObj stripped = BatchSafeWriter::stripNonWCInfo( gleResponse );
        ASSERT_EQUALS( stripped.nFields(), 1 );
        ASSERT( !stripped["ok"].trueValue() );
    }

    TEST(LegacyGLESuppress, ReplError) {

        const BSONObj gleResponse =
            fromjson( "{ok: 0.0, err: 'norepl', n: 1, wcField: true}" );

        BSONObj stripped = BatchSafeWriter::stripNonWCInfo( gleResponse );
        ASSERT_EQUALS( stripped.nFields(), 3 );
        ASSERT( !stripped["ok"].trueValue() );
        ASSERT_EQUALS( stripped["err"].str(), "norepl" );
        ASSERT( stripped["wcField"].trueValue() );
    }

    TEST(LegacyGLESuppress, StripCode) {

        const BSONObj gleResponse =
            fromjson( "{ok: 1.0, err: 'message', code: 12345}" );

        BSONObj stripped = BatchSafeWriter::stripNonWCInfo( gleResponse );
        ASSERT_EQUALS( stripped.nFields(), 2 ); // with err, ok : true
        ASSERT( stripped["ok"].trueValue() );
    }

    TEST(LegacyGLESuppress, TimeoutDupError24) {

        const BSONObj gleResponse =
            BSON( "ok" << 0.0 << "err" << "message" << "code" << 12345
                       << "err" << "timeout" << "code" << 56789 << "wtimeout" << true );

        BSONObj stripped = BatchSafeWriter::stripNonWCInfo( gleResponse );
        ASSERT_EQUALS( stripped.nFields(), 4 );
        ASSERT( !stripped["ok"].trueValue() );
        ASSERT_EQUALS( stripped["err"].str(), "timeout" );
        ASSERT_EQUALS( stripped["code"].numberInt(), 56789 );
        ASSERT( stripped["wtimeout"].trueValue() );
    }

    //
    // Tests of basic logical dispatching and aggregation for legacy GLE-based write concern
    //

    class MockCommandDispatch : public MultiCommandDispatch {
    public:

        MockCommandDispatch( const vector<BSONObj>& gleResponses ) :
            _gleResponses( gleResponses.begin(), gleResponses.end() ) {
        }

        virtual ~MockCommandDispatch() {
        }

        void addCommand( const ConnectionString& endpoint,
                         const StringData& dbName,
                         const BSONSerializable& request ) {
            _gleHosts.push_back( endpoint );
        }

        void sendAll() {
            // No-op
        }

        /**
         * Returns the number of sent requests that are still waiting to be recv'd.
         */
        int numPending() const {
            return _gleHosts.size();
        }

        Status recvAny( ConnectionString* endpoint, BSONSerializable* response ) {
            *endpoint = _gleHosts.front();
            response->parseBSON( _gleResponses.front(), NULL );
            _gleHosts.pop_front();
            _gleResponses.pop_front();
            return Status::OK();
        }

    private:

        deque<ConnectionString> _gleHosts;
        deque<BSONObj> _gleResponses;
    };

    TEST(LegacyGLEWriteConcern, Basic) {

        HostOpTimeMap hostOpTimes;
        hostOpTimes[ConnectionString::mock(HostAndPort("shardA:1000"))] = 
            std::make_pair(OpTime(), OID());
        hostOpTimes[ConnectionString::mock(HostAndPort("shardB:1000"))] =
            std::make_pair(OpTime(), OID());

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null}" ) );
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null}" ) );

        MockCommandDispatch dispatcher( gleResponses );
        vector<LegacyWCResponse> wcResponses;

        Status status = enforceLegacyWriteConcern( &dispatcher,
                                                   "db",
                                                   BSONObj(),
                                                   hostOpTimes,
                                                   &wcResponses );

        ASSERT_OK( status );
        ASSERT_EQUALS( wcResponses.size(), 2u );
    }

    TEST(LegacyGLEWriteConcern, FailGLE) {

        HostOpTimeMap hostOpTimes;
        hostOpTimes[ConnectionString::mock(HostAndPort("shardA:1000"))] = 
            std::make_pair(OpTime(), OID());
        hostOpTimes[ConnectionString::mock(HostAndPort("shardB:1000"))] =
            std::make_pair(OpTime(), OID());

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 0.0, errmsg: 'something'}" ) );
        gleResponses.push_back( fromjson( "{ok: 1.0, err: null}" ) );

        MockCommandDispatch dispatcher( gleResponses );
        vector<LegacyWCResponse> wcResponses;

        Status status = enforceLegacyWriteConcern( &dispatcher,
                                                   "db",
                                                   BSONObj(),
                                                   hostOpTimes,
                                                   &wcResponses );

        ASSERT_NOT_OK( status );
        // Ensure we keep getting the rest of the responses
        ASSERT_EQUALS( wcResponses.size(), 1u );
    }

    TEST(LegacyGLEWriteConcern, MultiWCErrors) {

        HostOpTimeMap hostOpTimes;
        hostOpTimes[ConnectionString::mock( HostAndPort( "shardA:1000" ) )] =
            std::make_pair(OpTime(), OID());
        hostOpTimes[ConnectionString::mock( HostAndPort( "shardB:1000" ) )] = 
            std::make_pair(OpTime(), OID());

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 0.0, err: 'norepl'}" ) );
        gleResponses.push_back( fromjson( "{ok: 0.0, err: 'timeout', wtimeout: true}" ) );

        MockCommandDispatch dispatcher( gleResponses );
        vector<LegacyWCResponse> wcResponses;

        Status status = enforceLegacyWriteConcern( &dispatcher,
                                                   "db",
                                                   BSONObj(),
                                                   hostOpTimes,
                                                   &wcResponses );

        ASSERT_OK( status );
        ASSERT_EQUALS( wcResponses.size(), 2u );
        ASSERT_EQUALS( wcResponses[0].shardHost, "shardA:1000" );
        ASSERT_EQUALS( wcResponses[0].gleResponse["err"].str(), "norepl" );
        ASSERT_EQUALS( wcResponses[0].errToReport, "norepl" );
        ASSERT_EQUALS( wcResponses[1].shardHost, "shardB:1000" );
        ASSERT_EQUALS( wcResponses[1].gleResponse["err"].str(), "timeout" );
        ASSERT_EQUALS( wcResponses[1].errToReport, "timeout" );
    }

    TEST(LegacyGLEWriteConcern, MultiFailGLE) {

        HostOpTimeMap hostOpTimes;
        hostOpTimes[ConnectionString::mock(HostAndPort("shardA:1000"))] = 
            std::make_pair(OpTime(), OID());
        hostOpTimes[ConnectionString::mock(HostAndPort("shardB:1000"))] =
            std::make_pair(OpTime(), OID());

        vector<BSONObj> gleResponses;
        gleResponses.push_back( fromjson( "{ok: 0.0, errmsg: 'something'}" ) );
        gleResponses.push_back( fromjson( "{ok: 0.0, errmsg: 'something'}" ) );

        MockCommandDispatch dispatcher( gleResponses );
        vector<LegacyWCResponse> wcResponses;

        Status status = enforceLegacyWriteConcern( &dispatcher,
                                                   "db",
                                                   BSONObj(),
                                                   hostOpTimes,
                                                   &wcResponses );

        ASSERT_NOT_OK( status );
        ASSERT_EQUALS( wcResponses.size(), 0u );
    }

}
