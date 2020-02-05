/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/repl/abstract_oplog_fetcher_test_fixture.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using namespace unittest;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using NetworkGuard = executor::NetworkInterfaceMock::InNetworkGuard;

class OplogFetcherTest : public AbstractOplogFetcherTest {
protected:
    void setUp() override;

    /**
     * Starts an oplog fetcher. Processes a single batch of results from
     * the oplog query and shuts down.
     * Returns shutdown state.
     */

    // 16MB max batch size / 12 byte min doc size * 10 (for good measure) = defaultBatchSize to use.
    const int defaultBatchSize = (16 * 1024 * 1024) / 12 * 10;

    std::unique_ptr<ShutdownState> processSingleBatch(executor::RemoteCommandResponse response,
                                                      bool requireFresherSyncSource = true);
    std::unique_ptr<ShutdownState> processSingleBatch(BSONObj obj,
                                                      bool requireFresherSyncSource = true);

    /**
     * Makes an OplogQueryMetadata object with the given fields and a stale committed OpTime.
     */
    BSONObj makeOplogQueryMetadataObject(OpTime lastAppliedOpTime,
                                         int rbid,
                                         int primaryIndex,
                                         int syncSourceIndex);

    /**
     * Tests checkSyncSource result handling.
     */
    void testSyncSourceChecking(rpc::ReplSetMetadata* replMetadata,
                                rpc::OplogQueryMetadata* oqMetadata);

    /**
     * Tests handling of two batches of operations returned from query.
     * Returns getMore request.
     */
    RemoteCommandRequest testTwoBatchHandling();

    OpTime remoteNewerOpTime;
    OpTime staleOpTime;
    Date_t staleWallTime;
    int rbid;

    std::unique_ptr<DataReplicatorExternalStateMock> dataReplicatorExternalState;

    Fetcher::Documents lastEnqueuedDocuments;
    OplogFetcher::DocumentsInfo lastEnqueuedDocumentsInfo;
    OplogFetcher::EnqueueDocumentsFn enqueueDocumentsFn;

    std::unique_ptr<OplogFetcher> makeOplogFetcher(ReplSetConfig config);
};

void OplogFetcherTest::setUp() {
    AbstractOplogFetcherTest::setUp();

    remoteNewerOpTime = {{124, 1}, 2};
    staleOpTime = {{1, 1}, 0};
    staleWallTime = Date_t() + Seconds(staleOpTime.getSecs());
    rbid = 2;

    dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateMock>();
    dataReplicatorExternalState->currentTerm = lastFetched.getTerm();
    dataReplicatorExternalState->lastCommittedOpTime = {{9999, 0}, lastFetched.getTerm()};

    enqueueDocumentsFn = [this](Fetcher::Documents::const_iterator begin,
                                Fetcher::Documents::const_iterator end,
                                const OplogFetcher::DocumentsInfo& info) -> Status {
        lastEnqueuedDocuments = {begin, end};
        lastEnqueuedDocumentsInfo = info;
        return Status::OK();
    };
}

BSONObj OplogFetcherTest::makeOplogQueryMetadataObject(OpTime lastAppliedOpTime,
                                                       int rbid,
                                                       int primaryIndex,
                                                       int syncSourceIndex) {
    rpc::OplogQueryMetadata oqMetadata(
        {staleOpTime, staleWallTime}, lastAppliedOpTime, rbid, primaryIndex, syncSourceIndex);
    BSONObjBuilder bob;
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    return bob.obj();
}

HostAndPort source("localhost:12345");
NamespaceString nss("local.oplog.rs");

ReplSetConfig _createConfig() {
    BSONObjBuilder bob;
    bob.append("_id", "myset");
    bob.append("version", 1);
    bob.append("protocolVersion", 1);
    {
        BSONArrayBuilder membersBob(bob.subarrayStart("members"));
        BSONObjBuilder(membersBob.subobjStart())
            .appendElements(BSON("_id" << 0 << "host" << source.toString()));
    }
    {
        BSONObjBuilder settingsBob(bob.subobjStart("settings"));
        settingsBob.append("electionTimeoutMillis", 10000);
    }
    auto configObj = bob.obj();

    ReplSetConfig config;
    ASSERT_OK(config.initialize(configObj));
    return config;
}

std::unique_ptr<ShutdownState> OplogFetcherTest::processSingleBatch(RemoteCommandResponse response,
                                                                    bool requireFresherSyncSource) {
    auto shutdownState = std::make_unique<ShutdownState>();

    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(),
                              0,
                              rbid,
                              requireFresherSyncSource,
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              std::ref(*shutdownState),
                              defaultBatchSize);

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    auto request = processNetworkResponse(response);

    ASSERT_BSONOBJ_EQ(oplogFetcher.getCommandObject_forTest(), request.cmdObj);
    ASSERT_BSONOBJ_EQ(oplogFetcher.getMetadataObject_forTest(), request.metadata);

    oplogFetcher.shutdown();
    oplogFetcher.join();

    return shutdownState;
}

std::unique_ptr<ShutdownState> OplogFetcherTest::processSingleBatch(BSONObj obj,
                                                                    bool requireFresherSyncSource) {
    return processSingleBatch({obj, Milliseconds(0)}, requireFresherSyncSource);
}

void _checkDefaultCommandObjectFields(BSONObj cmdObj) {
    ASSERT_EQUALS(std::string("find"), cmdObj.firstElementFieldName());
    ASSERT_TRUE(cmdObj.getBoolField("tailable"));
    ASSERT_TRUE(cmdObj.getBoolField("oplogReplay"));
    ASSERT_TRUE(cmdObj.getBoolField("awaitData"));
    ASSERT_EQUALS(60000, cmdObj.getIntField("maxTimeMS"));
}

std::unique_ptr<OplogFetcher> OplogFetcherTest::makeOplogFetcher(ReplSetConfig config) {
    return std::make_unique<OplogFetcher>(&getExecutor(),
                                          lastFetched,
                                          source,
                                          nss,
                                          config,
                                          0,
                                          -1,
                                          true,
                                          dataReplicatorExternalState.get(),
                                          enqueueDocumentsFn,
                                          [](Status) {},
                                          defaultBatchSize);
}

BSONObj concatenate(BSONObj a, const BSONObj& b) {
    auto bob = BSONObjBuilder(std::move(a));
    bob.appendElements(b);
    return bob.obj();
}

TEST_F(
    OplogFetcherTest,
    FindQueryContainsTermAndStartTimestampIfGetCurrentTermAndLastCommittedOpTimeReturnsValidTerm) {
    auto cmdObj = makeOplogFetcher(_createConfig())->getFindQuery_forTest();
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["filter"].type());
    ASSERT_BSONOBJ_EQ(BSON("ts" << BSON("$gte" << lastFetched.getTimestamp())),
                      cmdObj["filter"].Obj());
    ASSERT_EQUALS(dataReplicatorExternalState->currentTerm, cmdObj["term"].numberLong());
    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(OplogFetcherTest,
       FindQueryDoesNotContainTermIfGetCurrentTermAndLastCommittedOpTimeReturnsUninitializedTerm) {
    dataReplicatorExternalState->currentTerm = OpTime::kUninitializedTerm;
    auto cmdObj = makeOplogFetcher(_createConfig())->getFindQuery_forTest();
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["filter"].type());
    ASSERT_BSONOBJ_EQ(BSON("ts" << BSON("$gte" << lastFetched.getTimestamp())),
                      cmdObj["filter"].Obj());
    ASSERT_FALSE(cmdObj.hasField("term"));
    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(OplogFetcherTest, MetadataObjectContainsMetadataFieldsUnderProtocolVersion1) {
    auto metadataObj = makeOplogFetcher(_createConfig())->getMetadataObject_forTest();
    ASSERT_EQUALS(3, metadataObj.nFields());
    ASSERT_EQUALS(1, metadataObj[rpc::kReplSetMetadataFieldName].numberInt());
    ASSERT_EQUALS(1, metadataObj[rpc::kOplogQueryMetadataFieldName].numberInt());
}

TEST_F(OplogFetcherTest, AwaitDataTimeoutShouldEqualHalfElectionTimeoutUnderProtocolVersion1) {
    auto config = _createConfig();
    auto timeout = makeOplogFetcher(config)->getAwaitDataTimeout_forTest();
    ASSERT_EQUALS(config.getElectionTimeoutPeriod() / 2, timeout);
}

TEST_F(OplogFetcherTest, InvalidReplSetMetadataInResponseStopsTheOplogFetcher) {
    auto shutdownState =
        processSingleBatch({concatenate(makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                        BSON(rpc::kReplSetMetadataFieldName
                                             << BSON("invalid_repl_metadata_field" << 1))),
                            Milliseconds(0)});

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, shutdownState->getStatus());
}

TEST_F(OplogFetcherTest, InvalidOplogQueryMetadataInResponseStopsTheOplogFetcher) {
    auto shutdownState =
        processSingleBatch({concatenate(makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                        BSON(rpc::kOplogQueryMetadataFieldName
                                             << BSON("invalid_oq_metadata_field" << 1))),
                            Milliseconds(0)});

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, shutdownState->getStatus());
}

DEATH_TEST_F(OplogFetcherTest,
             ValidMetadataInResponseWithoutOplogMetadataInvariants,
             "Invariant failure oqMetadata") {
    rpc::ReplSetMetadata metadata(
        1, {lastFetched, lastFetchedWall}, lastFetched, 1, OID::gen(), 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(metadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    processSingleBatch(
        {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}), metadataObj),
         Milliseconds(0)});
}

TEST_F(OplogFetcherTest, ValidMetadataWithInResponseShouldBeForwardedToProcessMetadataFn) {
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata({staleOpTime, staleWallTime}, remoteNewerOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();
    ASSERT_OK(
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}), metadataObj),
             Milliseconds(0)})
            ->getStatus());
    ASSERT_TRUE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT_EQUALS(replMetadata.getPrimaryIndex(),
                  dataReplicatorExternalState->replMetadataProcessed.getPrimaryIndex());
    ASSERT_EQUALS(oqMetadata.getPrimaryIndex(),
                  dataReplicatorExternalState->oqMetadataProcessed.getPrimaryIndex());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreNotProcessedWhenSyncSourceRollsBack) {
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(
        {staleOpTime, staleWallTime}, remoteNewerOpTime, rbid + 1, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_EQUALS(
        ErrorCodes::InvalidSyncSource,
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}), metadataObj),
             Milliseconds(0)})
            ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreNotProcessedWhenSyncSourceIsBehind) {
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata({staleOpTime, staleWallTime}, staleOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_EQUALS(
        ErrorCodes::InvalidSyncSource,
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}), metadataObj),
             Milliseconds(0)})
            ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreNotProcessedWhenSyncSourceIsNotAhead) {
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata({staleOpTime, staleWallTime}, lastFetched, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_EQUALS(
        ErrorCodes::InvalidSyncSource,
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}), metadataObj),
             Milliseconds(0)})
            ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest,
       MetadataAndBatchAreNotProcessedWhenSyncSourceIsBehindWithoutRequiringFresherSyncSource) {
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata({staleOpTime, staleWallTime}, staleOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    auto entry = makeNoopOplogEntry(staleOpTime);
    ASSERT_EQUALS(
        ErrorCodes::InvalidSyncSource,
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {entry}), metadataObj), Milliseconds(0)}, false)
            ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreProcessedWhenSyncSourceIsCurrentButMetadataIsStale) {
    // This tests the case where the sync source metadata is behind us but we get a document which
    // is equal to us.  Since that means the metadata is stale and can be ignored, we should accept
    // this sync source.
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata({staleOpTime, staleWallTime}, staleOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    auto entry = makeNoopOplogEntry(lastFetched);
    auto shutdownState = processSingleBatch(
        {concatenate(makeCursorResponse(0, {entry}), metadataObj), Milliseconds(0)}, false);
    ASSERT_OK(shutdownState->getStatus());
    ASSERT(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest,
       MetadataAndBatchAreProcessedWhenSyncSourceIsNotAheadWithoutRequiringFresherSyncSource) {
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata({staleOpTime, staleWallTime}, lastFetched, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    auto entry = makeNoopOplogEntry(lastFetched);
    auto shutdownState = processSingleBatch(
        {concatenate(makeCursorResponse(0, {entry}), metadataObj), Milliseconds(0)}, false);
    ASSERT_OK(shutdownState->getStatus());
    ASSERT(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest,
       MetadataWithoutOplogQueryMetadataIsNotProcessedOnBatchThatTriggersRollback) {
    rpc::ReplSetMetadata metadata(
        1, {lastFetched, lastFetchedWall}, lastFetched, 1, OID::gen(), 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(metadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();
    ASSERT_EQUALS(
        ErrorCodes::OplogStartMissing,
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(Seconds(456))}), metadataObj),
             Milliseconds(0)})
            ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest, MetadataIsNotProcessedOnBatchThatTriggersRollback) {
    rpc::ReplSetMetadata replMetadata(1, {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata({staleOpTime, staleWallTime}, remoteNewerOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();
    ASSERT_EQUALS(
        ErrorCodes::OplogStartMissing,
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(Seconds(456))}), metadataObj),
             Milliseconds(0)})
            ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest, EmptyMetadataIsNotProcessed) {
    ASSERT_OK(processSingleBatch(
                  {makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}), Milliseconds(0)})
                  ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest, EmptyFirstBatchStopsOplogFetcherWithOplogStartMissingError) {
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch(makeCursorResponse(0, {}))->getStatus());
}

TEST_F(OplogFetcherTest, MissingOpTimeInFirstDocumentCausesOplogFetcherToStopWithInvalidBSONError) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(ErrorCodes::InvalidBSON,
                  processSingleBatch({concatenate(makeCursorResponse(0, {BSONObj()}), metadataObj),
                                      Milliseconds(0)})
                      ->getStatus());
}

TEST_F(
    OplogFetcherTest,
    LastOpTimeFetchedDoesNotMatchFirstDocumentCausesOplogFetcherToStopWithOplogStartMissingError) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(
        ErrorCodes::OplogStartMissing,
        processSingleBatch(
            {concatenate(makeCursorResponse(0, {makeNoopOplogEntry(Seconds(456))}), metadataObj),
             Milliseconds(0)})
            ->getStatus());
}

TEST_F(OplogFetcherTest,
       MissingOpTimeInSecondDocumentOfFirstBatchCausesOplogFetcherToStopWithNoSuchKey) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(
        ErrorCodes::NoSuchKey,
        processSingleBatch(
            {concatenate(makeCursorResponse(0,
                                            {makeNoopOplogEntry(lastFetched),
                                             BSON("o" << BSON("msg"
                                                              << "oplog entry without optime"))}),
                         metadataObj),
             Milliseconds(0)})
            ->getStatus());
}

TEST_F(OplogFetcherTest, TimestampsNotAdvancingInBatchCausesOplogFetcherStopWithOplogOutOfOrder) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(
        ErrorCodes::OplogOutOfOrder,
        processSingleBatch({concatenate(makeCursorResponse(0,
                                                           {makeNoopOplogEntry(lastFetched),
                                                            makeNoopOplogEntry(Seconds(1000)),
                                                            makeNoopOplogEntry(Seconds(2000)),
                                                            makeNoopOplogEntry(Seconds(1500))}),
                                        metadataObj),
                            Milliseconds(0)})
            ->getStatus());
}

TEST_F(OplogFetcherTest, OplogFetcherShouldExcludeFirstDocumentInFirstBatchWhenEnqueuingDocuments) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);

    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    Fetcher::Documents documents{firstEntry, secondEntry, thirdEntry};

    auto shutdownState = processSingleBatch(
        {concatenate(makeCursorResponse(0, documents), metadataObj), Milliseconds(0)});

    ASSERT_EQUALS(2U, lastEnqueuedDocuments.size());
    ASSERT_BSONOBJ_EQ(secondEntry, lastEnqueuedDocuments[0]);
    ASSERT_BSONOBJ_EQ(thirdEntry, lastEnqueuedDocuments[1]);

    ASSERT_EQUALS(3U, lastEnqueuedDocumentsInfo.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  lastEnqueuedDocumentsInfo.networkDocumentBytes);

    ASSERT_EQUALS(2U, lastEnqueuedDocumentsInfo.toApplyDocumentCount);
    ASSERT_EQUALS(size_t(secondEntry.objsize() + thirdEntry.objsize()),
                  lastEnqueuedDocumentsInfo.toApplyDocumentBytes);

    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)),
                  lastEnqueuedDocumentsInfo.lastDocument);

    // The last fetched optime should be updated after pushing the operations into the
    // buffer and reflected in the shutdown callback arguments.
    ASSERT_OK(shutdownState->getStatus());
}

TEST_F(OplogFetcherTest,
       OplogFetcherShouldNotDuplicateFirstDocWithEnqueueFirstDocOnErrorAfterFirstDoc) {

    // This function verifies that every oplog entry is only enqueued once.
    OpTime lastEnqueuedOpTime = OpTime();
    enqueueDocumentsFn = [&lastEnqueuedOpTime](Fetcher::Documents::const_iterator begin,
                                               Fetcher::Documents::const_iterator end,
                                               const OplogFetcher::DocumentsInfo&) -> Status {
        auto count = 0;
        auto toEnqueueOpTime = OpTime();

        for (auto i = begin; i != end; ++i) {
            count++;

            toEnqueueOpTime = OplogEntry(*i).getOpTime();
            ASSERT_GREATER_THAN(toEnqueueOpTime, lastEnqueuedOpTime);
            lastEnqueuedOpTime = toEnqueueOpTime;
        }

        ASSERT_EQ(1, count);
        return Status::OK();
    };

    auto shutdownState = std::make_unique<ShutdownState>();
    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(),
                              1 /* maxFetcherRestarts */,
                              rbid,
                              false /* requireFresherSyncSource */,
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              std::ref(*shutdownState),
                              defaultBatchSize,
                              OplogFetcher::StartingPoint::kEnqueueFirstDoc);

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    auto firstEntry = makeNoopOplogEntry({{Seconds(123), 0}, lastFetched.getTerm()});
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);

    // Only send over the first entry. Save the second for the getMore request.
    processNetworkResponse(
        {concatenate(makeCursorResponse(22L, {firstEntry}), metadataObj), Milliseconds(0)}, true);

    // Simulate an error right before receiving the second entry.
    auto request = processNetworkResponse(RemoteCommandResponse(ErrorCodes::QueryPlanKilled,
                                                                "Simulating failure for test.",
                                                                Milliseconds(0)),
                                          true);
    ASSERT_EQUALS(std::string("getMore"), request.cmdObj.firstElementFieldName());

    // Resend all data for the retry. The enqueueDocumentsFn will check to make sure that
    // the first entry was not enqueued twice.
    request = processNetworkResponse(
        {concatenate(makeCursorResponse(0, {firstEntry, secondEntry}), metadataObj),
         Milliseconds(0)},
        false);

    ASSERT_EQUALS(std::string("find"), request.cmdObj.firstElementFieldName());
    ASSERT_EQUALS("oplog.rs", request.cmdObj["find"].String());

    ASSERT(request.cmdObj["filter"].ok());
    ASSERT(request.cmdObj["filter"]["ts"].ok());
    ASSERT(request.cmdObj["filter"]["ts"]["$gte"].ok());
    ASSERT_EQUALS(firstEntry["ts"].timestamp(), request.cmdObj["filter"]["ts"]["$gte"].timestamp());

    oplogFetcher.join();
    ASSERT_OK(shutdownState->getStatus());
}

TEST_F(OplogFetcherTest,
       OplogFetcherShouldNotDuplicateFirstDocWithEnqueueFirstDocOnErrorAfterSecondDoc) {

    // This function verifies that every oplog entry is only enqueued once.
    OpTime lastEnqueuedOpTime = OpTime();
    enqueueDocumentsFn = [&lastEnqueuedOpTime](Fetcher::Documents::const_iterator begin,
                                               Fetcher::Documents::const_iterator end,
                                               const OplogFetcher::DocumentsInfo&) -> Status {
        auto count = 0;
        auto toEnqueueOpTime = OpTime();

        for (auto i = begin; i != end; ++i) {
            count++;

            toEnqueueOpTime = OplogEntry(*i).getOpTime();
            ASSERT_GREATER_THAN(toEnqueueOpTime, lastEnqueuedOpTime);
            lastEnqueuedOpTime = toEnqueueOpTime;
        }

        ASSERT_NOT_GREATER_THAN(count, 2);
        return Status::OK();
    };

    auto shutdownState = std::make_unique<ShutdownState>();
    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(),
                              1 /* maxFetcherRestarts */,
                              rbid,
                              false /* requireFresherSyncSource */,
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              std::ref(*shutdownState),
                              defaultBatchSize,
                              OplogFetcher::StartingPoint::kEnqueueFirstDoc);

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    auto firstEntry = makeNoopOplogEntry({{Seconds(123), 0}, lastFetched.getTerm()});
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);

    // Only send over the first two entries. Save the third for the getMore request.
    processNetworkResponse(
        {concatenate(makeCursorResponse(22L, {firstEntry, secondEntry}), metadataObj),
         Milliseconds(0)},
        true);

    // Simulate an error right before receiving the third entry.
    auto request = processNetworkResponse(RemoteCommandResponse(ErrorCodes::QueryPlanKilled,
                                                                "Simulating failure for test.",
                                                                Milliseconds(0)),
                                          true);
    ASSERT_EQUALS(std::string("getMore"), request.cmdObj.firstElementFieldName());

    // Resend all data for the retry. The enqueueDocumentsFn will check to make sure that
    // the first entry was not enqueued twice.
    request = processNetworkResponse(
        {concatenate(makeCursorResponse(0, {secondEntry, thirdEntry}), metadataObj),
         Milliseconds(0)},
        false);

    ASSERT_EQUALS(std::string("find"), request.cmdObj.firstElementFieldName());
    ASSERT_EQUALS("oplog.rs", request.cmdObj["find"].String());

    ASSERT(request.cmdObj["filter"].ok());
    ASSERT(request.cmdObj["filter"]["ts"].ok());
    ASSERT(request.cmdObj["filter"]["ts"]["$gte"].ok());
    ASSERT_EQUALS(secondEntry["ts"].timestamp(),
                  request.cmdObj["filter"]["ts"]["$gte"].timestamp());

    oplogFetcher.join();
    ASSERT_OK(shutdownState->getStatus());
}

TEST_F(OplogFetcherTest, OplogFetcherShouldReportErrorsThrownFromCallback) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);

    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    Fetcher::Documents documents{firstEntry, secondEntry, thirdEntry};

    enqueueDocumentsFn = [](Fetcher::Documents::const_iterator,
                            Fetcher::Documents::const_iterator,
                            const OplogFetcher::DocumentsInfo&) -> Status {
        return Status(ErrorCodes::InternalError, "my custom error");
    };

    auto shutdownState = processSingleBatch(
        {concatenate(makeCursorResponse(0, documents), metadataObj), Milliseconds(0)});
    ASSERT_EQ(shutdownState->getStatus(), Status(ErrorCodes::InternalError, "my custom error"));
}

void OplogFetcherTest::testSyncSourceChecking(rpc::ReplSetMetadata* replMetadata,
                                              rpc::OplogQueryMetadata* oqMetadata) {
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    Fetcher::Documents documents{firstEntry, secondEntry, thirdEntry};

    BSONObjBuilder bob;
    if (replMetadata) {
        ASSERT_OK(replMetadata->writeToMetadata(&bob));
    }
    if (oqMetadata) {
        ASSERT_OK(oqMetadata->writeToMetadata(&bob));
    }
    BSONObj metadataObj = bob.obj();

    dataReplicatorExternalState->shouldStopFetchingResult = true;

    auto shutdownState = processSingleBatch(
        {concatenate(makeCursorResponse(0, documents), metadataObj), Milliseconds(0)});

    // Sync source checking happens after we have successfully pushed the operations into
    // the buffer for the next replication phase (eg. applier).
    // The last fetched optime should be reflected in the shutdown callback arguments.
    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, shutdownState->getStatus());
}

TEST_F(OplogFetcherTest, FailedSyncSourceCheckWithoutMetadataStopsTheOplogFetcher) {
    testSyncSourceChecking(nullptr, nullptr);

    // Sync source optime and "hasSyncSource" are not available if the response does not
    // contain metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(OpTime(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_FALSE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

TEST_F(OplogFetcherTest, FailedSyncSourceCheckWithBothMetadatasStopsTheOplogFetcher) {
    rpc::ReplSetMetadata replMetadata(
        lastFetched.getTerm(), {OpTime(), Date_t()}, OpTime(), 1, OID::gen(), -1, -1);
    OpTime committedOpTime = {{Seconds(10000), 0}, 1};
    rpc::OplogQueryMetadata oqMetadata(
        {committedOpTime, Date_t() + Seconds(committedOpTime.getSecs())},
        {{Seconds(20000), 0}, 1},
        rbid,
        2,
        2);

    testSyncSourceChecking(&replMetadata, &oqMetadata);

    // Sync source optime and "hasSyncSource" can be set if the respone contains metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(oqMetadata.getLastOpApplied(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_TRUE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

TEST_F(OplogFetcherTest,
       FailedSyncSourceCheckWithSyncSourceHavingNoSyncSourceStopsTheOplogFetcher) {
    OpTime committedOpTime = {{Seconds(10000), 0}, 1};
    rpc::ReplSetMetadata replMetadata(
        lastFetched.getTerm(),
        {committedOpTime, Date_t() + Seconds(committedOpTime.getSecs())},
        {{Seconds(20000), 0}, 1},
        1,
        OID::gen(),
        2,
        2);
    rpc::OplogQueryMetadata oqMetadata(
        {committedOpTime, Date_t() + Seconds(committedOpTime.getSecs())},
        {{Seconds(20000), 0}, 1},
        rbid,
        2,
        -1);

    testSyncSourceChecking(&replMetadata, &oqMetadata);

    // Sync source "hasSyncSource" is derived from metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(oqMetadata.getLastOpApplied(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_FALSE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

RemoteCommandRequest OplogFetcherTest::testTwoBatchHandling() {
    ShutdownState shutdownState;

    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(),
                              0,
                              rbid,
                              true,
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              std::ref(shutdownState),
                              defaultBatchSize);
    ASSERT_EQUALS(OplogFetcher::State::kPreStart, oplogFetcher.getState_forTest());

    ASSERT_OK(oplogFetcher.startup());
    ASSERT_EQUALS(OplogFetcher::State::kRunning, oplogFetcher.getState_forTest());

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});

    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    processNetworkResponse(
        {concatenate(makeCursorResponse(cursorId, {firstEntry, secondEntry}), metadataObj),
         Milliseconds(0)},
        true);

    ASSERT_EQUALS(1U, lastEnqueuedDocuments.size());
    ASSERT_BSONOBJ_EQ(secondEntry, lastEnqueuedDocuments[0]);

    // Set cursor ID to 0 in getMore response to indicate no more data available.
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    auto fourthEntry = makeNoopOplogEntry({{Seconds(1200), 0}, lastFetched.getTerm()});
    auto request = processNetworkResponse(makeCursorResponse(0, {thirdEntry, fourthEntry}, false));

    ASSERT_EQUALS(std::string("getMore"), request.cmdObj.firstElementFieldName());
    ASSERT_EQUALS(nss.coll(), request.cmdObj["collection"].String());
    ASSERT_EQUALS(int(durationCount<Milliseconds>(oplogFetcher.getAwaitDataTimeout_forTest())),
                  request.cmdObj.getIntField("maxTimeMS"));

    ASSERT_EQUALS(2U, lastEnqueuedDocuments.size());
    ASSERT_BSONOBJ_EQ(thirdEntry, lastEnqueuedDocuments[0]);
    ASSERT_BSONOBJ_EQ(fourthEntry, lastEnqueuedDocuments[1]);

    oplogFetcher.join();
    ASSERT_EQUALS(OplogFetcher::State::kComplete, oplogFetcher.getState_forTest());

    ASSERT_OK(shutdownState.getStatus());

    return request;
}

TEST_F(
    OplogFetcherTest,
    NoDataAvailableAfterFirstTwoBatchesShouldCauseTheOplogFetcherToShutDownWithSuccessfulStatus) {
    auto request = testTwoBatchHandling();
    ASSERT_EQUALS(dataReplicatorExternalState->currentTerm, request.cmdObj["term"].numberLong());
    ASSERT_EQUALS(dataReplicatorExternalState->lastCommittedOpTime,
                  unittest::assertGet(OpTime::parseFromOplogEntry(
                      request.cmdObj["lastKnownCommittedOpTime"].Obj())));
}

TEST_F(OplogFetcherTest, ValidateDocumentsReturnsNoSuchKeyIfTimestampIsNotFoundInAnyDocument) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123));
    auto secondEntry = BSON("o" << BSON("msg"
                                        << "oplog entry without optime"));

    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  OplogFetcher::validateDocuments(
                      {firstEntry, secondEntry},
                      true,
                      unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp())
                      .getStatus());
}

TEST_F(
    OplogFetcherTest,
    ValidateDocumentsReturnsOutOfOrderIfTimestampInFirstEntryIsEqualToLastTimestampAndNotProcessingFirstBatch) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123));
    auto secondEntry = makeNoopOplogEntry(Seconds(456));

    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  OplogFetcher::validateDocuments(
                      {firstEntry, secondEntry},
                      false,
                      unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp())
                      .getStatus());
}

TEST_F(OplogFetcherTest, ValidateDocumentsReturnsOutOfOrderIfTimestampInSecondEntryIsBeforeFirst) {
    auto firstEntry = makeNoopOplogEntry(Seconds(456));
    auto secondEntry = makeNoopOplogEntry(Seconds(123));

    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  OplogFetcher::validateDocuments(
                      {firstEntry, secondEntry},
                      true,
                      unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp())
                      .getStatus());
}

TEST_F(OplogFetcherTest, ValidateDocumentsReturnsOutOfOrderIfTimestampInThirdEntryIsBeforeSecond) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123));
    auto secondEntry = makeNoopOplogEntry(Seconds(789));
    auto thirdEntry = makeNoopOplogEntry(Seconds(456));

    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  OplogFetcher::validateDocuments(
                      {firstEntry, secondEntry, thirdEntry},
                      true,
                      unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp())
                      .getStatus());
}

TEST_F(
    OplogFetcherTest,
    ValidateDocumentsExcludesFirstDocumentInApplyCountAndBytesIfProcessingFirstBatchAndSkipFirstDoc) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123));
    auto secondEntry = makeNoopOplogEntry(Seconds(456));
    auto thirdEntry = makeNoopOplogEntry(Seconds(789));

    auto info = unittest::assertGet(OplogFetcher::validateDocuments(
        {firstEntry, secondEntry, thirdEntry},
        true,
        unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp(),
        mongo::repl::OplogFetcher::StartingPoint::kSkipFirstDoc));

    ASSERT_EQUALS(3U, info.networkDocumentCount);
    ASSERT_EQUALS(2U, info.toApplyDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  info.networkDocumentBytes);
    ASSERT_EQUALS(size_t(secondEntry.objsize() + thirdEntry.objsize()), info.toApplyDocumentBytes);

    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)), info.lastDocument);
}

TEST_F(
    OplogFetcherTest,
    ValidateDocumentsIncludesFirstDocumentInApplyCountAndBytesIfProcessingFirstBatchAndEnqueueFirstDoc) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123));
    auto secondEntry = makeNoopOplogEntry(Seconds(456));
    auto thirdEntry = makeNoopOplogEntry(Seconds(789));

    auto info = unittest::assertGet(OplogFetcher::validateDocuments(
        {firstEntry, secondEntry, thirdEntry},
        true,
        unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp(),
        mongo::repl::OplogFetcher::StartingPoint::kEnqueueFirstDoc));

    ASSERT_EQUALS(3U, info.networkDocumentCount);
    ASSERT_EQUALS(3U, info.toApplyDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  info.networkDocumentBytes);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  info.toApplyDocumentBytes);

    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)), info.lastDocument);
}

TEST_F(OplogFetcherTest,
       ValidateDocumentsIncludesFirstDocumentInApplyCountAndBytesIfNotProcessingFirstBatch) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123));
    auto secondEntry = makeNoopOplogEntry(Seconds(456));
    auto thirdEntry = makeNoopOplogEntry(Seconds(789));

    auto info = unittest::assertGet(OplogFetcher::validateDocuments(
        {firstEntry, secondEntry, thirdEntry}, false, Timestamp(Seconds(100), 0)));

    ASSERT_EQUALS(3U, info.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  info.networkDocumentBytes);

    ASSERT_EQUALS(info.networkDocumentCount, info.toApplyDocumentCount);
    ASSERT_EQUALS(info.networkDocumentBytes, info.toApplyDocumentBytes);

    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)), info.lastDocument);
}

TEST_F(OplogFetcherTest,
       ValidateDocumentsReturnsDefaultLastDocumentOpTimeWhenThereAreNoDocumentsToApply) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123));

    auto info = unittest::assertGet(OplogFetcher::validateDocuments(
        {firstEntry},
        true,
        unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp()));

    ASSERT_EQUALS(1U, info.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize()), info.networkDocumentBytes);

    ASSERT_EQUALS(0U, info.toApplyDocumentCount);
    ASSERT_EQUALS(0U, info.toApplyDocumentBytes);

    ASSERT_EQUALS(OpTime(), info.lastDocument);
}

TEST_F(OplogFetcherTest,
       ValidateDocumentsReturnsOplogStartMissingWhenThereAreNoDocumentsWhenProcessingFirstBatch) {
    ASSERT_EQUALS(
        ErrorCodes::OplogStartMissing,
        OplogFetcher::validateDocuments({}, true, Timestamp(Seconds(100), 0)).getStatus());
}

TEST_F(OplogFetcherTest,
       ValidateDocumentsReturnsDefaultInfoWhenThereAreNoDocumentsWhenNotProcessingFirstBatch) {
    auto info =
        unittest::assertGet(OplogFetcher::validateDocuments({}, false, Timestamp(Seconds(100), 0)));

    ASSERT_EQUALS(0U, info.networkDocumentCount);
    ASSERT_EQUALS(0U, info.networkDocumentBytes);

    ASSERT_EQUALS(0U, info.toApplyDocumentCount);
    ASSERT_EQUALS(0U, info.toApplyDocumentBytes);

    ASSERT_EQUALS(OpTime(), info.lastDocument);
}

BSONObj makeNoopOplogEntry(OpTime opTime) {
    auto oplogEntry =
        repl::OplogEntry(opTime,                           // optime
                         boost::none,                      // hash
                         OpTypeEnum ::kNoop,               // opType
                         NamespaceString("test.t"),        // namespace
                         boost::none,                      // uuid
                         boost::none,                      // fromMigrate
                         repl::OplogEntry::kOplogVersion,  // version
                         BSONObj(),                        // o
                         boost::none,                      // o2
                         {},                               // sessionInfo
                         boost::none,                      // upsert
                         Date_t(),                         // wall clock time
                         boost::none,                      // statement id
                         boost::none,   // optime of previous write within same transaction
                         boost::none,   // pre-image optime
                         boost::none);  // post-image optime
    return oplogEntry.toBSON();
}

BSONObj makeNoopOplogEntry(Seconds seconds) {
    return makeNoopOplogEntry({{seconds, 0}, 1LL});
}

BSONObj makeOplogBatchMetadata(boost::optional<const rpc::ReplSetMetadata&> replMetadata,
                               boost::optional<const rpc::OplogQueryMetadata&> oqMetadata) {
    BSONObjBuilder bob;
    if (replMetadata) {
        ASSERT_OK(replMetadata->writeToMetadata(&bob));
    }
    if (oqMetadata) {
        ASSERT_OK(oqMetadata->writeToMetadata(&bob));
    }
    return bob.obj();
}

Message makeFirstBatch(CursorId cursorId,
                       const NewOplogFetcher::Documents& oplogEntries,
                       const BSONObj& metadata) {
    return MockDBClientConnection::mockFindResponse(
        NamespaceString::kRsOplogNamespace, cursorId, oplogEntries, metadata);
}

Message makeSubsequentBatch(CursorId cursorId,
                            const NewOplogFetcher::Documents& oplogEntries,
                            const BSONObj& metadata,
                            bool moreToCome) {
    return MockDBClientConnection::mockGetMoreResponse(
        NamespaceString::kRsOplogNamespace, cursorId, oplogEntries, metadata, moreToCome);
}

bool blockedOnNetworkSoon(MockDBClientConnection* conn) {
    // Wait up to 10 seconds.
    for (auto i = 0; i < 100; i++) {
        if (conn->isBlockedOnNetwork()) {
            return true;
        }
        mongo::sleepmillis(100);
    }
    return false;
}

void validateFindCommand(Message m, OpTime lastFetched, int findTimeout) {
    auto msg = mongo::OpMsg::parse(m);
    ASSERT_EQ(mongo::StringData(msg.body.firstElement().fieldName()), "find");
    ASSERT_TRUE(msg.body.getBoolField("tailable"));
    ASSERT_TRUE(msg.body.getBoolField("oplogReplay"));
    ASSERT_TRUE(msg.body.getBoolField("awaitData"));
    ASSERT_EQUALS(findTimeout, msg.body.getIntField("maxTimeMS"));
    ASSERT_BSONOBJ_EQ(BSON("ts" << BSON("$gte" << lastFetched.getTimestamp())),
                      msg.body.getObjectField("filter"));
    ASSERT_EQUALS(lastFetched.getTerm(), msg.body.getIntField("term"));
    ASSERT_BSONOBJ_EQ(BSON("level"
                           << "local"
                           << "afterClusterTime" << Timestamp(0, 1)),
                      msg.body.getObjectField("readConcern"));
    // TODO SERVER-45470: Test the metadata sent.
}

void validateGetMoreCommand(Message m, int cursorId, int timeout, bool exhaustSupported = true) {
    auto msg = mongo::OpMsg::parse(m);
    ASSERT_EQ(mongo::StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(cursorId, msg.body.getIntField("getMore"));
    ASSERT_EQUALS(timeout, msg.body.getIntField("maxTimeMS"));
    if (exhaustSupported) {
        ASSERT_TRUE(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    } else {
        ASSERT_FALSE(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    }
}

// Simulate a response to a single outgoing client request and return the client request. Use this
// function to simulate responses to client find/getMore requests.
Message processSingleRequestResponse(DBClientConnection* conn,
                                     const StatusWith<Message> response,
                                     bool expectReadyNetworkOperationsAfterProcessing = false) {
    auto* mockConn = dynamic_cast<MockDBClientConnection*>(conn);
    ASSERT_TRUE(blockedOnNetworkSoon(mockConn));
    auto request = mockConn->getLastSentMessage();
    mockConn->setCallResponses({response});
    if (expectReadyNetworkOperationsAfterProcessing) {
        ASSERT_TRUE(blockedOnNetworkSoon(mockConn));
    }
    return request;
}

// Simulate a response to a single network recv() call. Use this function to simulate responses to
// exhaust stream where a client expects to receive responses without sending out new requests.
void processSingleExhaustResponse(DBClientConnection* conn,
                                  const StatusWith<Message> response,
                                  bool expectReadyNetworkOperationsAfterProcessing = false) {
    auto* mockConn = dynamic_cast<MockDBClientConnection*>(conn);
    ASSERT_TRUE(blockedOnNetworkSoon(mockConn));
    mockConn->setRecvResponses({response});
    if (expectReadyNetworkOperationsAfterProcessing) {
        ASSERT_TRUE(blockedOnNetworkSoon(mockConn));
    }
}

void simulateNetworkDisconnect(DBClientConnection* conn) {
    auto* mockConn = dynamic_cast<MockDBClientConnection*>(conn);
    ASSERT_TRUE(blockedOnNetworkSoon(mockConn));
    mockConn->shutdown();
}

class NewOplogFetcherTest : public executor::ThreadPoolExecutorTest,
                            public ScopedGlobalServiceContextForTest {
protected:
    static const OpTime remoteNewerOpTime;
    static const OpTime staleOpTime;
    static const Date_t staleWallTime;
    static const int rbid = 2;
    static const int primaryIndex = 2;
    static const int syncSourceIndex = 2;
    static const rpc::OplogQueryMetadata staleOqMetadata;

    // 16MB max batch size / 12 byte min doc size * 10 (for good measure) = defaultBatchSize to use.
    const int defaultBatchSize = (16 * 1024 * 1024) / 12 * 10;

    void setUp() override;

    std::unique_ptr<NewOplogFetcher> makeOplogFetcher();
    std::unique_ptr<NewOplogFetcher> makeOplogFetcherWithDifferentExecutor(
        executor::TaskExecutor* executor,
        NewOplogFetcher::OnShutdownCallbackFn fn,
        int numRestarts = 0,
        bool requireFresherSyncSource = true);
    std::unique_ptr<NewOplogFetcher> getOplogFetcherAfterConnectionCreated(
        NewOplogFetcher::OnShutdownCallbackFn fn,
        int numRestarts = 0,
        bool requireFresherSyncSource = true);

    std::unique_ptr<ShutdownState> processSingleBatch(const Message& response,
                                                      bool requireFresherSyncSource = true);

    /**
     * Tests checkSyncSource result handling.
     */
    void testSyncSourceChecking(boost::optional<const rpc::ReplSetMetadata&> replMetadata,
                                boost::optional<const rpc::OplogQueryMetadata&> oqMetadata);

    std::unique_ptr<DataReplicatorExternalStateMock> dataReplicatorExternalState;

    NewOplogFetcher::Documents lastEnqueuedDocuments;
    NewOplogFetcher::DocumentsInfo lastEnqueuedDocumentsInfo;
    NewOplogFetcher::EnqueueDocumentsFn enqueueDocumentsFn;

    // The last OpTime fetched by the oplog fetcher.
    OpTime lastFetched;

    std::unique_ptr<MockRemoteDBServer> _mockServer;
};

const OpTime NewOplogFetcherTest::remoteNewerOpTime = OpTime(Timestamp(124, 1), 2);
const OpTime NewOplogFetcherTest::staleOpTime = OpTime(Timestamp(1, 1), 0);
const Date_t NewOplogFetcherTest::staleWallTime = Date_t() + Seconds(staleOpTime.getSecs());
const rpc::OplogQueryMetadata NewOplogFetcherTest::staleOqMetadata = rpc::OplogQueryMetadata(
    {staleOpTime, staleWallTime}, staleOpTime, rbid, primaryIndex, syncSourceIndex);

void NewOplogFetcherTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    launchExecutorThread();

    lastFetched = {{123, 0}, 1};

    dataReplicatorExternalState = std::make_unique<DataReplicatorExternalStateMock>();
    dataReplicatorExternalState->currentTerm = lastFetched.getTerm();
    dataReplicatorExternalState->lastCommittedOpTime = {{9999, 0}, lastFetched.getTerm()};

    enqueueDocumentsFn = [this](NewOplogFetcher::Documents::const_iterator begin,
                                NewOplogFetcher::Documents::const_iterator end,
                                const NewOplogFetcher::DocumentsInfo& info) -> Status {
        lastEnqueuedDocuments = {begin, end};
        lastEnqueuedDocumentsInfo = info;
        return Status::OK();
    };

    auto target = HostAndPort{"localhost:12346"};
    _mockServer = std::make_unique<MockRemoteDBServer>(target.toString());
}

std::unique_ptr<NewOplogFetcher> NewOplogFetcherTest::makeOplogFetcher() {
    return makeOplogFetcherWithDifferentExecutor(&getExecutor(), [](Status) {});
}

std::unique_ptr<NewOplogFetcher> NewOplogFetcherTest::getOplogFetcherAfterConnectionCreated(
    NewOplogFetcher::OnShutdownCallbackFn fn, int numRestarts, bool requireFresherSyncSource) {
    auto oplogFetcher = makeOplogFetcherWithDifferentExecutor(
        &getExecutor(), fn, numRestarts, requireFresherSyncSource);

    auto waitForConnCreatedFailPoint =
        globalFailPointRegistry().find("logAfterOplogFetcherConnCreated");
    auto timesEnteredFailPoint = waitForConnCreatedFailPoint->setMode(FailPoint::alwaysOn, 0);

    ASSERT_FALSE(oplogFetcher->isActive());
    ASSERT_OK(oplogFetcher->startup());
    ASSERT_TRUE(oplogFetcher->isActive());

    // Ensure that the MockDBClientConnection was created before proceeding.
    waitForConnCreatedFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    return oplogFetcher;
}

std::unique_ptr<NewOplogFetcher> NewOplogFetcherTest::makeOplogFetcherWithDifferentExecutor(
    executor::TaskExecutor* executor,
    NewOplogFetcher::OnShutdownCallbackFn fn,
    int numRestarts,
    bool requireFresherSyncSource) {
    auto oplogFetcher = std::make_unique<NewOplogFetcher>(
        executor,
        lastFetched,
        source,
        _createConfig(),
        std::make_unique<NewOplogFetcher::OplogFetcherRestartDecisionDefault>(numRestarts),
        -1,
        requireFresherSyncSource,
        dataReplicatorExternalState.get(),
        enqueueDocumentsFn,
        fn,
        defaultBatchSize,
        NewOplogFetcher::StartingPoint::kSkipFirstDoc);
    oplogFetcher->setCreateClientFn_forTest([this]() {
        const auto autoReconnect = true;
        return std::unique_ptr<DBClientConnection>(
            new MockDBClientConnection(_mockServer.get(), autoReconnect));
    });
    return oplogFetcher;
}

std::unique_ptr<ShutdownState> NewOplogFetcherTest::processSingleBatch(
    const Message& response, bool requireFresherSyncSource) {
    auto shutdownState = std::make_unique<ShutdownState>();

    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(
        std::ref(*shutdownState), 0, requireFresherSyncSource);

    auto m = processSingleRequestResponse(oplogFetcher->getDBClientConnection_forTest(), response);
    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest()));

    oplogFetcher->shutdown();
    oplogFetcher->join();

    return shutdownState;
}

void NewOplogFetcherTest::testSyncSourceChecking(
    boost::optional<const rpc::ReplSetMetadata&> replMetadata,
    boost::optional<const rpc::OplogQueryMetadata&> oqMetadata) {
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});

    auto metadataObj = makeOplogBatchMetadata(replMetadata, oqMetadata);

    dataReplicatorExternalState->shouldStopFetchingResult = true;

    auto shutdownState =
        processSingleBatch(makeFirstBatch(0, {firstEntry, secondEntry, thirdEntry}, metadataObj));

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, shutdownState->getStatus());
}

TEST_F(NewOplogFetcherTest, ShuttingExecutorDownShouldPreventOplogFetcherFromStarting) {
    getExecutor().shutdown();

    auto oplogFetcher = makeOplogFetcher();

    // Last optime fetched should match values passed to constructor.
    ASSERT_EQUALS(lastFetched, oplogFetcher->getLastOpTimeFetched_forTest());

    ASSERT_FALSE(oplogFetcher->isActive());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, oplogFetcher->startup());
    ASSERT_FALSE(oplogFetcher->isActive());

    // Last optime fetched should not change.
    ASSERT_EQUALS(lastFetched, oplogFetcher->getLastOpTimeFetched_forTest());
}

TEST_F(NewOplogFetcherTest, OplogFetcherReturnsOperationFailedIfExecutorFailsToScheduleRunQuery) {
    TaskExecutorMock taskExecutorMock(&getExecutor());
    taskExecutorMock.shouldFailScheduleWorkRequest = []() { return true; };

    // The onShutdownFn should not be called because the oplog fetcher should fail during startup.
    auto oplogFetcher =
        makeOplogFetcherWithDifferentExecutor(&taskExecutorMock, [](Status) { MONGO_UNREACHABLE; });

    // Last optime fetched should match values passed to constructor.
    ASSERT_EQUALS(lastFetched, oplogFetcher->getLastOpTimeFetched_forTest());

    ASSERT_FALSE(oplogFetcher->isActive());
    ASSERT_EQUALS(ErrorCodes::OperationFailed, oplogFetcher->startup());
    ASSERT_FALSE(oplogFetcher->isActive());

    // Last optime fetched should not change.
    ASSERT_EQUALS(lastFetched, oplogFetcher->getLastOpTimeFetched_forTest());
}

TEST_F(NewOplogFetcherTest, ShuttingExecutorDownAfterStartupButBeforeRunQueryScheduled) {
    ShutdownState shutdownState;

    // Defer scheduling work so that the executor's shutdown happens before startup's work is
    // scheduled.
    TaskExecutorMock taskExecutorMock(&getExecutor());
    taskExecutorMock.shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };

    auto oplogFetcher =
        makeOplogFetcherWithDifferentExecutor(&taskExecutorMock, std::ref(shutdownState));

    ASSERT_FALSE(oplogFetcher->isActive());
    ASSERT_OK(oplogFetcher->startup());
    ASSERT_TRUE(oplogFetcher->isActive());

    getExecutor().shutdown();

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherReturnsCallbackCanceledIfShutdownBeforeRunQueryScheduled) {
    ShutdownState shutdownState;

    // Defer scheduling work so that the oplog fetcher's shutdown happens before startup's work is
    // scheduled.
    TaskExecutorMock taskExecutorMock(&getExecutor());
    taskExecutorMock.shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };

    auto oplogFetcher =
        makeOplogFetcherWithDifferentExecutor(&taskExecutorMock, std::ref(shutdownState));

    ASSERT_FALSE(oplogFetcher->isActive());
    ASSERT_OK(oplogFetcher->startup());
    ASSERT_TRUE(oplogFetcher->isActive());

    oplogFetcher->shutdown();

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherReturnsCallbackCanceledIfShutdownAfterRunQueryScheduled) {
    // Tests shutting down after _runQuery is scheduled (but not while blocked on the network).

    ShutdownState shutdownState;

    auto waitForCallbackScheduledFailPoint =
        globalFailPointRegistry().find("hangAfterOplogFetcherCallbackScheduled");
    auto timesEnteredFailPoint = waitForCallbackScheduledFailPoint->setMode(FailPoint::alwaysOn, 0);

    // This will also ensure that _runQuery was scheduled before returning.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState));

    waitForCallbackScheduledFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Only call shutdown once we have confirmed that the callback is paused at the fail point.
    oplogFetcher->shutdown();

    // Unpause the oplog fetcher.
    waitForCallbackScheduledFailPoint->setMode(FailPoint::off);

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest,
       OplogFetcherReturnsHostUnreachableIfShutdownAfterRunQueryScheduledWhileBlockedOnCall) {
    // Tests that shutting down while the connection is blocked on call successfully shuts down the
    // connection as well.

    ShutdownState shutdownState;

    // This will also ensure that _runQuery was scheduled before returning.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState));

    // Make sure that we are blocked on the network before shutting down to make sure that shutting
    // down properly interrupts and shuts down the DBClientConnection.
    auto* mockConn =
        dynamic_cast<MockDBClientConnection*>(oplogFetcher->getDBClientConnection_forTest());
    ASSERT_TRUE(blockedOnNetworkSoon(mockConn));

    oplogFetcher->shutdown();

    oplogFetcher->join();

    // This is the error that the connection throws if shutdown while blocked on the network.
    ASSERT_EQUALS(ErrorCodes::HostUnreachable, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherReturnsHostUnreachableOnConnectionFailures) {
    // Test that OplogFetcher fails to establish initial connection, retrying HostUnreachable.
    ShutdownState shutdownState;

    // Shutdown the mock remote server before the OplogFetcher tries to connect.
    _mockServer->shutdown();

    // This will also ensure that _runQuery was scheduled before returning.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState));

    oplogFetcher->join();

    // This is the error code for connection failures.
    ASSERT_EQUALS(ErrorCodes::HostUnreachable, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherRetriesConnectionButFails) {
    // Test that OplogFetcher tries but fails after failing the initial connection, retrying
    // HostUnreachable.
    ShutdownState shutdownState;

    // Shutdown the mock remote server before the OplogFetcher tries to connect.
    _mockServer->shutdown();

    // Create an OplogFetcher with 1 retry attempt. This will also ensure that _runQuery was
    // scheduled before returning.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    oplogFetcher->join();

    // This is the error code for connection failures.
    ASSERT_EQUALS(ErrorCodes::HostUnreachable, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherResetsNumRestartsOnSuccessfulConnection) {
    // Test that OplogFetcher resets the number of restarts after a successful connection on a
    // retry.
    ShutdownState shutdownState;

    // Shutdown the mock remote server before the OplogFetcher tries to connect.
    _mockServer->shutdown();

    // Hang OplogFetcher before it retries the connection.
    auto beforeRetryingConnection = globalFailPointRegistry().find("hangBeforeOplogFetcherRetries");
    auto timesEntered = beforeRetryingConnection->setMode(FailPoint::alwaysOn);

    // Create an OplogFetcher with 1 retry attempt. This will also ensure that _runQuery was
    // scheduled before returning.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    // Wait until the first connect attempt fails but before it retries.
    beforeRetryingConnection->waitForTimesEntered(timesEntered + 1);

    // Bring up the mock server so that the connection retry can succeed.
    _mockServer->reboot();

    // Disable the failpoint to allow reconnection.
    beforeRetryingConnection->setMode(FailPoint::off);

    // After the connection succeeded, the number of restarts should be reset back to 0 so that the
    // OplogFetcher can tolerate another failure before failing. This will cause the first attempt
    // to create a cursor to fail. After this, the new cursor will be blocked on call() while
    // retrying to initialize. This also makes sure the OplogFetcher reconnects correctly.
    simulateNetworkDisconnect(oplogFetcher->getDBClientConnection_forTest());

    CursorId cursorId = 0LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // Allow the cursor re-initialization to succeed. But the OplogFetcher will shut down with an OK
    // status after receiving this batch because the cursor id is 0.
    processSingleRequestResponse(oplogFetcher->getDBClientConnection_forTest(),
                                 makeFirstBatch(cursorId, {firstEntry}, metadataObj));

    oplogFetcher->join();

    ASSERT_OK(shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherCanAutoReconnect) {
    // Test that the OplogFetcher can autoreconnect after a broken connection.
    ShutdownState shutdownState;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    auto conn = oplogFetcher->getDBClientConnection_forTest();
    // Simulate a disconnect for the first find command.
    simulateNetworkDisconnect(conn);

    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(124), 0}, lastFetched.getTerm()});
    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // Simulate closing the cursor and the OplogFetcher should exit with an OK status.
    processSingleRequestResponse(conn, makeFirstBatch(0LL, {firstEntry}, metadataObj));

    oplogFetcher->join();

    ASSERT_OK(shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherAutoReconnectsButFails) {
    // Test that the OplogFetcher fails an autoreconnect after a broken connection.
    ShutdownState shutdownState;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    auto conn = oplogFetcher->getDBClientConnection_forTest();
    // Shut down the mock server and simulate a disconnect for the first find command. And the
    // OplogFetcher should retry with AutoReconnect.
    _mockServer->shutdown();
    simulateNetworkDisconnect(conn);

    oplogFetcher->join();

    // AutoReconnect should also fail because the server is shut down.
    ASSERT_EQUALS(ErrorCodes::HostUnreachable, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, DisconnectsOnErrorsDuringExhaustStream) {
    // Test that the connection disconnects if we get errors after successfully receiving a batch
    // from the exhaust stream.
    ShutdownState shutdownState;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(124), 0}, lastFetched.getTerm()});
    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    auto conn = oplogFetcher->getDBClientConnection_forTest();
    // First batch for the initial find command.
    processSingleRequestResponse(conn, makeFirstBatch(cursorId, {firstEntry}, metadataObj), true);

    auto beforeRecreatingCursor = globalFailPointRegistry().find("hangBeforeOplogFetcherRetries");
    auto timesEntered = beforeRecreatingCursor->setMode(FailPoint::alwaysOn);

    // Temporarily override the metatdata reader to introduce failure after successfully receiving a
    // batch from the first getMore. And the exhaust stream is now established.
    conn->setReplyMetadataReader(
        [&](OperationContext* opCtx, const BSONObj& metadataObj, StringData target) {
            return Status(ErrorCodes::FailedToParse, "Fake error");
        });
    processSingleRequestResponse(
        conn, makeSubsequentBatch(cursorId, {secondEntry}, metadataObj, true /* moreToCome */));

    beforeRecreatingCursor->waitForTimesEntered(timesEntered + 1);

    // Test that the connection is disconnected because we cannot use the same connection to
    // recreate cursor as more data is on the way from the server for the exhaust stream.
    ASSERT_TRUE(conn->isFailed());

    // Unset the metatdata reader.
    conn->setReplyMetadataReader(rpc::ReplyMetadataReader());

    // Allow retry and autoreconnect.
    beforeRecreatingCursor->setMode(FailPoint::off);

    // Simulate closing the cursor and the OplogFetcher should exit with an OK status.
    processSingleRequestResponse(conn, makeFirstBatch(0LL, {firstEntry}, metadataObj));

    oplogFetcher->join();

    ASSERT_OK(shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest,
       FindQueryContainsTermIfGetCurrentTermAndLastCommittedOpTimeReturnsValidTerm) {
    // Test that the correct maxTimeMS is set if this is the initial 'find' query.
    auto oplogFetcher = makeOplogFetcher();
    auto findTimeout = durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest());
    auto queryObj = oplogFetcher->getFindQuery_forTest(findTimeout);
    ASSERT_EQUALS(60000, queryObj.getIntField("$maxTimeMS"));

    ASSERT_EQUALS(mongo::BSONType::Object, queryObj["query"].type());
    ASSERT_BSONOBJ_EQ(BSON("ts" << BSON("$gte" << lastFetched.getTimestamp())),
                      queryObj["query"].Obj());
    ASSERT_EQUALS(mongo::BSONType::Object, queryObj["readConcern"].type());
    ASSERT_BSONOBJ_EQ(BSON("level"
                           << "local"
                           << "afterClusterTime" << Timestamp(0, 1)),
                      queryObj["readConcern"].Obj());
    ASSERT_EQUALS(dataReplicatorExternalState->currentTerm, queryObj["term"].numberLong());
}

TEST_F(NewOplogFetcherTest,
       FindQueryDoesNotContainTermIfGetCurrentTermAndLastCommittedOpTimeReturnsUninitializedTerm) {
    dataReplicatorExternalState->currentTerm = OpTime::kUninitializedTerm;
    auto oplogFetcher = makeOplogFetcher();

    // Test that the correct maxTimeMS is set if we are retrying the 'find' query.
    auto findTimeout = durationCount<Milliseconds>(oplogFetcher->getRetriedFindMaxTime_forTest());
    auto queryObj = oplogFetcher->getFindQuery_forTest(findTimeout);
    ASSERT_EQUALS(2000, queryObj.getIntField("$maxTimeMS"));

    ASSERT_EQUALS(mongo::BSONType::Object, queryObj["query"].type());
    ASSERT_BSONOBJ_EQ(BSON("ts" << BSON("$gte" << lastFetched.getTimestamp())),
                      queryObj["query"].Obj());
    ASSERT_EQUALS(mongo::BSONType::Object, queryObj["readConcern"].type());
    ASSERT_BSONOBJ_EQ(BSON("level"
                           << "local"
                           << "afterClusterTime" << Timestamp(0, 1)),
                      queryObj["readConcern"].Obj());
    ASSERT_FALSE(queryObj.hasField("term"));
}

TEST_F(NewOplogFetcherTest, AwaitDataTimeoutShouldEqualHalfElectionTimeout) {
    auto config = _createConfig();
    auto timeout = makeOplogFetcher()->getAwaitDataTimeout_forTest();
    ASSERT_EQUALS(config.getElectionTimeoutPeriod() / 2, timeout);
}

TEST_F(NewOplogFetcherTest, AwaitDataTimeoutSmallerWhenFailPointSet) {
    auto failPoint = globalFailPointRegistry().find("setSmallOplogGetMoreMaxTimeMS");
    failPoint->setMode(FailPoint::alwaysOn);
    auto timeout = makeOplogFetcher()->getAwaitDataTimeout_forTest();
    ASSERT_EQUALS(Milliseconds(50), timeout);
    failPoint->setMode(FailPoint::off);
}

TEST_F(NewOplogFetcherTest, FailingInitialCreateNewCursorNoRetriesShutsDownOplogFetcher) {
    ShutdownState shutdownState;

    // Create an oplog fetcher without any retries.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState));

    // Use an empty message so that the cursor's init function will fail.
    processSingleRequestResponse(oplogFetcher->getDBClientConnection_forTest(), Message());

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, FailingInitialCreateNewCursorWithRetriesShutsDownOplogFetcher) {
    ShutdownState shutdownState;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    // An empty message will cause the initial attempt to create a cursor to fail.
    processSingleRequestResponse(oplogFetcher->getDBClientConnection_forTest(), Message(), true);

    // An empty message will cause the attempt to recreate a cursor to fail.
    processSingleRequestResponse(oplogFetcher->getDBClientConnection_forTest(), Message());

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest,
       NetworkExceptionDuringInitialCreateNewCursorWithRetriesShutsDownOplogFetcher) {
    ShutdownState shutdownState;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    // This will cause the initial attempt to create a cursor to fail.
    processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"},
        true);

    // This will cause the attempt to recreate a cursor to fail.
    processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"});

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, DontRecreateNewCursorAfterFailedBatchNoRetries) {
    ShutdownState shutdownState;

    // Create an oplog fetcher without any retries.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState));

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // Creating the cursor will succeed.
    auto m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {firstEntry, secondEntry}, metadataObj),
        true);

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest()));

    // This will cause the oplog fetcher to fail while getting the next batch. Since it doesn't have
    // any retries, the oplog fetcher will shut down.
    processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"});

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, FailCreateNewCursorAfterFailedBatchRetriesShutsDownOplogFetcher) {
    ShutdownState shutdownState;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // Creating the cursor will succeed.
    auto m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {firstEntry, secondEntry}, metadataObj),
        true);

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest()));

    // This will cause us to fail getting the next batch, meaning a new cursor needs to be created.
    processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"},
        true);

    // An empty message will cause the attempt to create a cursor to fail.
    processSingleRequestResponse(oplogFetcher->getDBClientConnection_forTest(), Message());

    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, SuccessfullyRecreateCursorAfterFailedBatch) {
    // This tests that the oplog fetcher successfully can recreate a cursor after it failed to get
    // a batch and makes sure the recreated cursor behaves like an exhaust cursor. This will also
    // check that the socket timeouts are set as expected. The steps are:
    // 1. Start the oplog fetcher.
    // 2. Create the initial cursor successfully.
    // 3. Fail getting the next batch, causing us to create a new cursor.
    // 4. Succeed creating a new cursor.
    // 5. Successfully get the next batch (from the first getMore command).
    // 6. Successfully get the next batch (this is the first exhaust batch).
    // 7. Shut down while the connection is blocked on the network.

    ShutdownState shutdownState;

    // -- Step 1 --
    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    // Make sure we are blocked on the network before checking that the socket timeout is properly
    // set.
    auto conn = oplogFetcher->getDBClientConnection_forTest();
    auto* mockConn = dynamic_cast<MockDBClientConnection*>(conn);
    ASSERT_TRUE(blockedOnNetworkSoon(mockConn));

    auto initialMaxFindTimeDouble =
        durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest()) / 1000.0;
    auto retriedMaxFindTimeDouble =
        durationCount<Milliseconds>(oplogFetcher->getRetriedFindMaxTime_forTest()) / 1000.0;
    auto awaitDataTimeoutDouble =
        durationCount<Milliseconds>(makeOplogFetcher()->getAwaitDataTimeout_forTest()) / 1000.0;

    // Check the socket timeout is equal to the initial find max time plus the network buffer.
    ASSERT_EQUALS(initialMaxFindTimeDouble + oplogNetworkTimeoutBufferSeconds.load(),
                  conn->getSoTimeout());

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(124), 0}, lastFetched.getTerm()});
    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // -- Step 2 --
    // Creating the cursor will succeed. After this, the cursor will be blocked on call() for the
    // getMore command.
    auto m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {firstEntry, secondEntry}, metadataObj),
        true);

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest()));

    // Check the socket timeout is equal to the awaitData timeout plus the network buffer.
    ASSERT_EQUALS(awaitDataTimeoutDouble + oplogNetworkTimeoutBufferSeconds.load(),
                  conn->getSoTimeout());

    // -- Step 3 --
    // This will cause us to fail getting the next batch, meaning a new cursor needs to be created.
    // After this, the new cursor will be blocked on call() while trying to initialize.
    m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"},
        true);

    validateGetMoreCommand(
        m, cursorId, durationCount<Milliseconds>(oplogFetcher->getAwaitDataTimeout_forTest()));

    // Check the socket timeout is equal to the retried find max time plus the network buffer.
    ASSERT_EQUALS(retriedMaxFindTimeDouble + oplogNetworkTimeoutBufferSeconds.load(),
                  conn->getSoTimeout());

    cursorId = 23LL;
    auto thirdEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});
    auto fourthEntry = makeNoopOplogEntry({{Seconds(457), 0}, lastFetched.getTerm()});

    // -- Step 4 --
    // This will cause the attempt to create a cursor to succeed. After this, the cursor will be
    // blocked on call() for the getMore command.
    m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {thirdEntry, fourthEntry}, metadataObj),
        true);

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getRetriedFindMaxTime_forTest()));

    // Check the socket timeout is equal to the awaitData timeout plus the network buffer.
    ASSERT_EQUALS(awaitDataTimeoutDouble + oplogNetworkTimeoutBufferSeconds.load(),
                  conn->getSoTimeout());

    auto fifthEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    auto sixthEntry = makeNoopOplogEntry({{Seconds(790), 0}, lastFetched.getTerm()});

    // -- Step 5 --
    // This will be the first getMore. After this, the cursor will be blocked on recv() for the next
    // batch.
    m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeSubsequentBatch(cursorId, {fifthEntry, sixthEntry}, metadataObj, true /* moreToCome */),
        true);

    validateGetMoreCommand(
        m, cursorId, durationCount<Milliseconds>(oplogFetcher->getAwaitDataTimeout_forTest()));

    auto seventhEntry = makeNoopOplogEntry({{Seconds(791), 0}, lastFetched.getTerm()});
    auto eighthEntry = makeNoopOplogEntry({{Seconds(792), 0}, lastFetched.getTerm()});

    // -- Step 6 --
    // Getting this batch will mean the cursor was successfully recreated as an exhaust cursor. The
    // moreToCome flag is set to false so that _connectionHasPendingReplies on the cursor will be
    // false when cleaning up the cursor (otherwise we'd need to use a side connection to clean up
    // the cursor).
    processSingleExhaustResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeSubsequentBatch(
            cursorId, {seventhEntry, eighthEntry}, metadataObj, false /* moreToCome */),
        true);

    // TODO SERVER-45469: Update this test to check the contents of the batches.

    // -- Step 7 --
    oplogFetcher->shutdown();
    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, SuccessfulBatchResetsNumRestarts) {
    // This tests that the OplogFetcherRestartDecision resets its counter when the oplog fetcher
    // successfully gets the next batch. The steps are:
    // 1. Start the oplog fetcher.
    // 2. Fail to create the initial cursor. This will increment the number of failed restarts.
    // 3. Create the cursor successfully. This should reset the count of failed restarts.
    // 4. Fail getting the next batch, causing us to create a new cursor.
    // 5. Succeed creating a new cursor.
    // 6. Shut down while the connection is blocked on the network.

    ShutdownState shutdownState;

    // -- Step 1 --
    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    // -- Step 2 --
    // This will cause the first attempt to create a cursor to fail. After this, the new cursor will
    // be blocked on call() while trying to initialize.
    processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"},
        true);

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});

    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // -- Step 3 --
    // Creating the cursor will succeed. After this, the cursor will be blocked on call() for the
    // getMore command.
    auto m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {firstEntry, secondEntry}, metadataObj),
        true);

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getRetriedFindMaxTime_forTest()));

    auto thirdEntry = makeNoopOplogEntry({{Seconds(457), 0}, lastFetched.getTerm()});
    auto fourthEntry = makeNoopOplogEntry({{Seconds(458), 0}, lastFetched.getTerm()});

    // -- Step 4 --
    // This will cause an error when getting the next batch, which will cause us to recreate the
    // cursor. If the number of retries was not reset, this will fail because the new cursor won't
    // be blocked on the call while recreating the cursor.
    processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        mongo::Status{mongo::ErrorCodes::NetworkTimeout, "Fake socket timeout"},
        true);

    cursorId = 23LL;
    auto fifthEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    auto sixthEntry = makeNoopOplogEntry({{Seconds(790), 0}, lastFetched.getTerm()});

    // -- Step 5 --
    // Make sure that we can finish recreating the cursor successfully.
    m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {fifthEntry, sixthEntry}, metadataObj),
        true);

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getRetriedFindMaxTime_forTest()));

    oplogFetcher->shutdown();
    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, OplogFetcherWorksWithoutExhaust) {
    // Test that the oplog fetcher works if the 'oplogFetcherUsesExhaust' server parameter is set to
    // false.

    ShutdownState shutdownState;

    oplogFetcherUsesExhaust = false;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.getTerm()});

    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // Creating the cursor will succeed. After this, the cursor will be blocked on call() for the
    // getMore command.
    auto m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {firstEntry, secondEntry}, metadataObj),
        true);

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest()));

    auto thirdEntry = makeNoopOplogEntry({{Seconds(457), 0}, lastFetched.getTerm()});
    auto fourthEntry = makeNoopOplogEntry({{Seconds(458), 0}, lastFetched.getTerm()});

    // moreToCome would be set to false if oplogFetcherUsesExhaust was set to false. After this,
    // the cursor will be blocked on call() for the next getMore command.
    m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeSubsequentBatch(
            cursorId, {thirdEntry, fourthEntry}, metadataObj, false /* moreToCome */),
        true);

    validateGetMoreCommand(m,
                           cursorId,
                           durationCount<Milliseconds>(oplogFetcher->getAwaitDataTimeout_forTest()),
                           false /* exhaustSupported */);

    // TODO SERVER-45469: Update this test to check the contents of the batches.

    auto fifthEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.getTerm()});
    auto sixthEntry = makeNoopOplogEntry({{Seconds(790), 0}, lastFetched.getTerm()});

    m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeSubsequentBatch(cursorId, {fifthEntry, sixthEntry}, metadataObj, false),
        true);

    validateGetMoreCommand(m,
                           cursorId,
                           durationCount<Milliseconds>(oplogFetcher->getAwaitDataTimeout_forTest()),
                           false /* exhaustSupported */);

    oplogFetcher->shutdown();
    oplogFetcher->join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
}

TEST_F(NewOplogFetcherTest, CursorIsDeadShutsDownOplogFetcherWithSuccessfulStatus) {
    ShutdownState shutdownState;

    // Create an oplog fetcher with one retry.
    auto oplogFetcher = getOplogFetcherAfterConnectionCreated(std::ref(shutdownState), 1);

    CursorId cursorId = 0LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(124), 0}, lastFetched.getTerm()});

    auto metadataObj = makeOplogBatchMetadata(boost::none, staleOqMetadata);

    // Creating the cursor will succeed, but the oplog fetcher will shut down after receiving this
    // batch because the cursor id is 0.
    auto m = processSingleRequestResponse(
        oplogFetcher->getDBClientConnection_forTest(),
        makeFirstBatch(cursorId, {firstEntry, secondEntry}, metadataObj));

    validateFindCommand(m,
                        oplogFetcher->getLastOpTimeFetched_forTest(),
                        durationCount<Milliseconds>(oplogFetcher->getInitialFindMaxTime_forTest()));

    oplogFetcher->join();

    ASSERT_OK(shutdownState.getStatus());
}
}  // namespace
