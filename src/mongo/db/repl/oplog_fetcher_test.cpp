/**
 *    Copyright 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/repl/abstract_oplog_fetcher_test_fixture.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/ensure_fcv.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
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
    RemoteCommandRequest testTwoBatchHandling(bool isV1ElectionProtocol);

    OpTime remoteNewerOpTime;
    OpTime staleOpTime;
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
    rbid = 2;

    dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
    dataReplicatorExternalState->currentTerm = lastFetched.opTime.getTerm();
    dataReplicatorExternalState->lastCommittedOpTime = {{9999, 0}, lastFetched.opTime.getTerm()};

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
        staleOpTime, lastAppliedOpTime, rbid, primaryIndex, syncSourceIndex);
    BSONObjBuilder bob;
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    return bob.obj();
}

HostAndPort source("localhost:12345");
NamespaceString nss("local.oplog.rs");

ReplSetConfig _createConfig(bool isV1ElectionProtocol) {
    BSONObjBuilder bob;
    bob.append("_id", "myset");
    bob.append("version", 1);
    if (isV1ElectionProtocol) {
        bob.append("protocolVersion", 1);
    }
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
    auto shutdownState = stdx::make_unique<ShutdownState>();

    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(true),
                              0,
                              rbid,
                              requireFresherSyncSource,
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              stdx::ref(*shutdownState),
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
    return processSingleBatch({obj, rpc::makeEmptyMetadata(), Milliseconds(0)},
                              requireFresherSyncSource);
}

void _checkDefaultCommandObjectFields(BSONObj cmdObj) {
    ASSERT_EQUALS(std::string("find"), cmdObj.firstElementFieldName());
    ASSERT_TRUE(cmdObj.getBoolField("tailable"));
    ASSERT_TRUE(cmdObj.getBoolField("oplogReplay"));
    ASSERT_TRUE(cmdObj.getBoolField("awaitData"));
    ASSERT_EQUALS(60000, cmdObj.getIntField("maxTimeMS"));
}

std::unique_ptr<OplogFetcher> OplogFetcherTest::makeOplogFetcher(ReplSetConfig config) {
    return stdx::make_unique<OplogFetcher>(&getExecutor(),
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

TEST_F(
    OplogFetcherTest,
    FindQueryContainsTermAndStartTimestampIfGetCurrentTermAndLastCommittedOpTimeReturnsValidTerm) {
    auto cmdObj = makeOplogFetcher(_createConfig(true))->getFindQuery_forTest();
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["filter"].type());
    ASSERT_BSONOBJ_EQ(BSON("ts" << BSON("$gte" << lastFetched.opTime.getTimestamp())),
                      cmdObj["filter"].Obj());
    ASSERT_EQUALS(dataReplicatorExternalState->currentTerm, cmdObj["term"].numberLong());
    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(OplogFetcherTest,
       FindQueryDoesNotContainTermIfGetCurrentTermAndLastCommittedOpTimeReturnsUninitializedTerm) {
    dataReplicatorExternalState->currentTerm = OpTime::kUninitializedTerm;
    auto cmdObj = makeOplogFetcher(_createConfig(true))->getFindQuery_forTest();
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["filter"].type());
    ASSERT_BSONOBJ_EQ(BSON("ts" << BSON("$gte" << lastFetched.opTime.getTimestamp())),
                      cmdObj["filter"].Obj());
    ASSERT_FALSE(cmdObj.hasField("term"));
    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(OplogFetcherTest, MetadataObjectContainsMetadataFieldsUnderProtocolVersion1) {
    auto metadataObj = makeOplogFetcher(_createConfig(true))->getMetadataObject_forTest();
    ASSERT_EQUALS(3, metadataObj.nFields());
    ASSERT_EQUALS(1, metadataObj[rpc::kReplSetMetadataFieldName].numberInt());
    ASSERT_EQUALS(1, metadataObj[rpc::kOplogQueryMetadataFieldName].numberInt());
}

TEST_F(OplogFetcherTest, MetadataObjectIsEmptyUnderProtocolVersion0) {
    auto metadataObj = makeOplogFetcher(_createConfig(false))->getMetadataObject_forTest();
    ASSERT_BSONOBJ_EQ(ReadPreferenceSetting::secondaryPreferredMetadata(), metadataObj);
}

TEST_F(OplogFetcherTest, AwaitDataTimeoutShouldEqualHalfElectionTimeoutUnderProtocolVersion1) {
    auto config = _createConfig(true);
    auto timeout = makeOplogFetcher(config)->getAwaitDataTimeout_forTest();
    ASSERT_EQUALS(config.getElectionTimeoutPeriod() / 2, timeout);
}

TEST_F(OplogFetcherTest, AwaitDataTimeoutShouldBeAConstantUnderProtocolVersion0) {
    auto timeout = makeOplogFetcher(_createConfig(false))->getAwaitDataTimeout_forTest();
    ASSERT_EQUALS(OplogFetcher::kDefaultProtocolZeroAwaitDataTimeout, timeout);
}

TEST_F(OplogFetcherTest, FindQueryContainsReadConcernIfTermNotLastFetched) {
    EnsureFCV ensureFCV(EnsureFCV::Version::kFullyUpgradedTo36);
    auto uninitializedTerm = OpTime::kUninitializedTerm;
    ASSERT_NOT_EQUALS(dataReplicatorExternalState->currentTerm, uninitializedTerm);
    dataReplicatorExternalState->currentTerm++;
    auto cmdObj = makeOplogFetcher(_createConfig(true))->getFindQuery_forTest();
    ASSERT_TRUE(cmdObj.hasField("readConcern")) << cmdObj;
    ASSERT_EQUALS(lastFetched.opTime,
                  OpTime::parse(cmdObj["readConcern"].Obj()["afterOpTime"].Obj()))
        << cmdObj;
}

TEST_F(OplogFetcherTest, FindQueryContainsReadConcernIfTermUninitialized) {
    EnsureFCV ensureFCV(EnsureFCV::Version::kFullyUpgradedTo36);
    dataReplicatorExternalState->currentTerm = OpTime::kUninitializedTerm;
    auto cmdObj = makeOplogFetcher(_createConfig(true))->getFindQuery_forTest();
    ASSERT_TRUE(cmdObj.hasField("readConcern")) << cmdObj;
    ASSERT_EQUALS(lastFetched.opTime,
                  OpTime::parse(cmdObj["readConcern"].Obj()["afterOpTime"].Obj()))
        << cmdObj;
}

TEST_F(OplogFetcherTest, FindQueryHasAfterOpTimeWithFeatureCompatibilityVersion34) {
    EnsureFCV ensureFCV(EnsureFCV::Version::kFullyDowngradedTo34);
    ASSERT(serverGlobalParams.featureCompatibility.getVersion() !=
           ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);
    auto cmdObj = makeOplogFetcher(_createConfig(true))->getFindQuery_forTest();
    auto readConcernElem = cmdObj["readConcern"];
    ASSERT_EQUALS(mongo::BSONType::Object, readConcernElem.type());
    ASSERT_FALSE(readConcernElem.Obj().hasField("afterClusterTime"));
    ASSERT_BSONOBJ_EQ(readConcernElem.Obj(), BSON("afterOpTime" << lastFetched.opTime));

    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(OplogFetcherTest, FindQueryHasAfterOpTimeWithFeatureCompatibilityVersion36) {
    EnsureFCV ensureFCV(EnsureFCV::Version::kFullyUpgradedTo36);
    ASSERT(serverGlobalParams.featureCompatibility.getVersion() ==
           ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);
    auto cmdObj = makeOplogFetcher(_createConfig(true))->getFindQuery_forTest();
    auto readConcernElem = cmdObj["readConcern"];
    ASSERT_EQUALS(mongo::BSONType::Object, readConcernElem.type());
    ASSERT_TRUE(readConcernElem.Obj().hasField("afterOpTime"));
    ASSERT_BSONOBJ_EQ(readConcernElem.Obj(), BSON("afterOpTime" << lastFetched.opTime.toBSON()));

    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(OplogFetcherTest, InvalidReplSetMetadataInResponseStopsTheOplogFetcher) {
    auto shutdownState = processSingleBatch(
        {makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
         BSON(rpc::kReplSetMetadataFieldName << BSON("invalid_repl_metadata_field" << 1)),
         Milliseconds(0)});

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, shutdownState->getStatus());
}

TEST_F(OplogFetcherTest, InvalidOplogQueryMetadataInResponseStopsTheOplogFetcher) {
    auto shutdownState = processSingleBatch(
        {makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
         BSON(rpc::kOplogQueryMetadataFieldName << BSON("invalid_oq_metadata_field" << 1)),
         Milliseconds(0)});

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, shutdownState->getStatus());
}

TEST_F(OplogFetcherTest,
       ValidMetadataInResponseWithoutOplogMetadataShouldBeForwardedToProcessMetadataFn) {
    rpc::ReplSetMetadata metadata(1, lastFetched.opTime, lastFetched.opTime, 1, OID::gen(), 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(metadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_OK(processSingleBatch({makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                  metadataObj,
                                  Milliseconds(0)})
                  ->getStatus());
    ASSERT_TRUE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT_EQUALS(metadata.getPrimaryIndex(),
                  dataReplicatorExternalState->replMetadataProcessed.getPrimaryIndex());
    ASSERT_EQUALS(-1, dataReplicatorExternalState->oqMetadataProcessed.getPrimaryIndex());
}

TEST_F(OplogFetcherTest, ValidMetadataWithInResponseShouldBeForwardedToProcessMetadataFn) {
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, remoteNewerOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();
    ASSERT_OK(processSingleBatch({makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                  metadataObj,
                                  Milliseconds(0)})
                  ->getStatus());
    ASSERT_TRUE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT_EQUALS(replMetadata.getPrimaryIndex(),
                  dataReplicatorExternalState->replMetadataProcessed.getPrimaryIndex());
    ASSERT_EQUALS(oqMetadata.getPrimaryIndex(),
                  dataReplicatorExternalState->oqMetadataProcessed.getPrimaryIndex());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreNotProcessedWhenSyncSourceRollsBack) {
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, remoteNewerOpTime, rbid + 1, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource,
                  processSingleBatch({makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                      metadataObj,
                                      Milliseconds(0)})
                      ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreNotProcessedWhenSyncSourceIsBehind) {
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, staleOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource,
                  processSingleBatch({makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                      metadataObj,
                                      Milliseconds(0)})
                      ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreNotProcessedWhenSyncSourceIsNotAhead) {
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, lastFetched.opTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource,
                  processSingleBatch({makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                      metadataObj,
                                      Milliseconds(0)})
                      ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest,
       MetadataAndBatchAreNotProcessedWhenSyncSourceIsBehindWithoutRequiringFresherSyncSource) {
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, staleOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    ASSERT_EQUALS(
        ErrorCodes::InvalidSyncSource,
        processSingleBatch({makeCursorResponse(0, {}), metadataObj, Milliseconds(0)}, false)
            ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
    ASSERT(lastEnqueuedDocuments.empty());
}

TEST_F(OplogFetcherTest, MetadataAndBatchAreProcessedWhenSyncSourceIsCurrentButMetadataIsStale) {
    // This tests the case where the sync source metadata is behind us but we get a document which
    // is equal to us.  Since that means the metadata is stale and can be ignored, we should accept
    // this sync source.
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, staleOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    auto entry = makeNoopOplogEntry(lastFetched);
    auto shutdownState =
        processSingleBatch({makeCursorResponse(0, {entry}), metadataObj, Milliseconds(0)}, false);
    ASSERT_OK(shutdownState->getStatus());
    ASSERT(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest,
       MetadataAndBatchAreProcessedWhenSyncSourceIsNotAheadWithoutRequiringFresherSyncSource) {
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, lastFetched.opTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();

    auto entry = makeNoopOplogEntry(lastFetched);
    auto shutdownState =
        processSingleBatch({makeCursorResponse(0, {entry}), metadataObj, Milliseconds(0)}, false);
    ASSERT_OK(shutdownState->getStatus());
    ASSERT(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest,
       MetadataWithoutOplogQueryMetadataIsNotProcessedOnBatchThatTriggersRollback) {
    rpc::ReplSetMetadata metadata(1, lastFetched.opTime, lastFetched.opTime, 1, OID::gen(), 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(metadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch(
                      {makeCursorResponse(0, {makeNoopOplogEntry(Seconds(456), lastFetched.value)}),
                       metadataObj,
                       Milliseconds(0)})
                      ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest, MetadataIsNotProcessedOnBatchThatTriggersRollback) {
    rpc::ReplSetMetadata replMetadata(1, OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(staleOpTime, remoteNewerOpTime, rbid, 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(replMetadata.writeToMetadata(&bob));
    ASSERT_OK(oqMetadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch(
                      {makeCursorResponse(0, {makeNoopOplogEntry(Seconds(456), lastFetched.value)}),
                       metadataObj,
                       Milliseconds(0)})
                      ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest, EmptyMetadataIsNotProcessed) {
    ASSERT_OK(processSingleBatch({makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}),
                                  rpc::makeEmptyMetadata(),
                                  Milliseconds(0)})
                  ->getStatus());
    ASSERT_FALSE(dataReplicatorExternalState->metadataWasProcessed);
}

TEST_F(OplogFetcherTest, EmptyFirstBatchStopsOplogFetcherWithOplogStartMissingError) {
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch(makeCursorResponse(0, {}))->getStatus());
}

TEST_F(OplogFetcherTest, MissingOpTimeInFirstDocumentCausesOplogFetcherToStopWithInvalidBSONError) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(
        ErrorCodes::InvalidBSON,
        processSingleBatch({makeCursorResponse(0, {BSONObj()}), metadataObj, Milliseconds(0)})
            ->getStatus());
}

TEST_F(
    OplogFetcherTest,
    LastOpTimeFetchedDoesNotMatchFirstDocumentCausesOplogFetcherToStopWithOplogStartMissingError) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch(
                      {makeCursorResponse(0, {makeNoopOplogEntry(Seconds(456), lastFetched.value)}),
                       metadataObj,
                       Milliseconds(0)})
                      ->getStatus());
}

TEST_F(OplogFetcherTest,
       LastHashFetchedDoesNotMatchFirstDocumentCausesOplogFetcherToStopWithOplogStartMissingError) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(
        ErrorCodes::OplogStartMissing,
        processSingleBatch(
            {makeCursorResponse(0, {makeNoopOplogEntry(remoteNewerOpTime, lastFetched.value + 1)}),
             metadataObj,
             Milliseconds(0)})
            ->getStatus());
}

TEST_F(OplogFetcherTest,
       MissingOpTimeInSecondDocumentOfFirstBatchCausesOplogFetcherToStopWithNoSuchKey) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  processSingleBatch(
                      {makeCursorResponse(0,
                                          {makeNoopOplogEntry(lastFetched),
                                           BSON("o" << BSON("msg"
                                                            << "oplog entry without optime"))}),
                       metadataObj,
                       Milliseconds(0)})
                      ->getStatus());
}

TEST_F(OplogFetcherTest, TimestampsNotAdvancingInBatchCausesOplogFetcherStopWithOplogOutOfOrder) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  processSingleBatch({makeCursorResponse(0,
                                                         {makeNoopOplogEntry(lastFetched),
                                                          makeNoopOplogEntry(Seconds(1000), 1),
                                                          makeNoopOplogEntry(Seconds(2000), 1),
                                                          makeNoopOplogEntry(Seconds(1500), 1)}),
                                      metadataObj,
                                      Milliseconds(0)})
                      ->getStatus());
}

TEST_F(OplogFetcherTest, OplogFetcherShouldExcludeFirstDocumentInFirstBatchWhenEnqueuingDocuments) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);

    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.opTime.getTerm()}, 200);
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.opTime.getTerm()}, 300);
    Fetcher::Documents documents{firstEntry, secondEntry, thirdEntry};

    auto shutdownState =
        processSingleBatch({makeCursorResponse(0, documents), metadataObj, Milliseconds(0)});

    ASSERT_EQUALS(2U, lastEnqueuedDocuments.size());
    ASSERT_BSONOBJ_EQ(secondEntry, lastEnqueuedDocuments[0]);
    ASSERT_BSONOBJ_EQ(thirdEntry, lastEnqueuedDocuments[1]);

    ASSERT_EQUALS(3U, lastEnqueuedDocumentsInfo.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  lastEnqueuedDocumentsInfo.networkDocumentBytes);

    ASSERT_EQUALS(2U, lastEnqueuedDocumentsInfo.toApplyDocumentCount);
    ASSERT_EQUALS(size_t(secondEntry.objsize() + thirdEntry.objsize()),
                  lastEnqueuedDocumentsInfo.toApplyDocumentBytes);

    ASSERT_EQUALS(thirdEntry["h"].numberLong(), lastEnqueuedDocumentsInfo.lastDocument.value);
    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)),
                  lastEnqueuedDocumentsInfo.lastDocument.opTime);

    // The last fetched optime and hash should be updated after pushing the operations into the
    // buffer and reflected in the shutdown callback arguments.
    ASSERT_OK(shutdownState->getStatus());
}

TEST_F(OplogFetcherTest, OplogFetcherShouldReportErrorsThrownFromCallback) {
    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);

    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.opTime.getTerm()}, 200);
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.opTime.getTerm()}, 300);
    Fetcher::Documents documents{firstEntry, secondEntry, thirdEntry};

    enqueueDocumentsFn = [](Fetcher::Documents::const_iterator,
                            Fetcher::Documents::const_iterator,
                            const OplogFetcher::DocumentsInfo&) -> Status {
        return Status(ErrorCodes::InternalError, "my custom error");
    };

    auto shutdownState =
        processSingleBatch({makeCursorResponse(0, documents), metadataObj, Milliseconds(0)});
    ASSERT_EQ(shutdownState->getStatus(), Status(ErrorCodes::InternalError, "my custom error"));
}

void OplogFetcherTest::testSyncSourceChecking(rpc::ReplSetMetadata* replMetadata,
                                              rpc::OplogQueryMetadata* oqMetadata) {
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.opTime.getTerm()}, 200);
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.opTime.getTerm()}, 300);
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

    auto shutdownState =
        processSingleBatch({makeCursorResponse(0, documents), metadataObj, Milliseconds(0)});

    // Sync source checking happens after we have successfully pushed the operations into
    // the buffer for the next replication phase (eg. applier).
    // The last fetched optime and hash should be reflected in the shutdown callback
    // arguments.
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

TEST_F(OplogFetcherTest, FailedSyncSourceCheckWithReplSetMetadataStopsTheOplogFetcher) {
    rpc::ReplSetMetadata metadata(lastFetched.opTime.getTerm(),
                                  {{Seconds(10000), 0}, 1},
                                  {{Seconds(20000), 0}, 1},
                                  1,
                                  OID::gen(),
                                  2,
                                  2);

    testSyncSourceChecking(&metadata, nullptr);

    // Sync source optime and "hasSyncSource" can be set if the respone contains metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(metadata.getLastOpVisible(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_TRUE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

TEST_F(OplogFetcherTest, FailedSyncSourceCheckWithBothMetadatasStopsTheOplogFetcher) {
    rpc::ReplSetMetadata replMetadata(
        lastFetched.opTime.getTerm(), OpTime(), OpTime(), 1, OID::gen(), -1, -1);
    rpc::OplogQueryMetadata oqMetadata(
        {{Seconds(10000), 0}, 1}, {{Seconds(20000), 0}, 1}, rbid, 2, 2);

    testSyncSourceChecking(&replMetadata, &oqMetadata);

    // Sync source optime and "hasSyncSource" can be set if the respone contains metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(oqMetadata.getLastOpApplied(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_TRUE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

TEST_F(OplogFetcherTest,
       FailedSyncSourceCheckWithSyncSourceHavingNoSyncSourceInReplSetMetadataStopsTheOplogFetcher) {
    rpc::ReplSetMetadata metadata(lastFetched.opTime.getTerm(),
                                  {{Seconds(10000), 0}, 1},
                                  {{Seconds(20000), 0}, 1},
                                  1,
                                  OID::gen(),
                                  2,
                                  -1);

    testSyncSourceChecking(&metadata, nullptr);

    // Sync source "hasSyncSource" is derived from metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(metadata.getLastOpVisible(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_FALSE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

TEST_F(OplogFetcherTest,
       FailedSyncSourceCheckWithSyncSourceHavingNoSyncSourceStopsTheOplogFetcher) {
    rpc::ReplSetMetadata replMetadata(lastFetched.opTime.getTerm(),
                                      {{Seconds(10000), 0}, 1},
                                      {{Seconds(20000), 0}, 1},
                                      1,
                                      OID::gen(),
                                      2,
                                      2);
    rpc::OplogQueryMetadata oqMetadata(
        {{Seconds(10000), 0}, 1}, {{Seconds(20000), 0}, 1}, rbid, 2, -1);

    testSyncSourceChecking(&replMetadata, &oqMetadata);

    // Sync source "hasSyncSource" is derived from metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(oqMetadata.getLastOpApplied(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_FALSE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

RemoteCommandRequest OplogFetcherTest::testTwoBatchHandling(bool isV1ElectionProtocol) {
    ShutdownState shutdownState;

    if (!isV1ElectionProtocol) {
        dataReplicatorExternalState->currentTerm = OpTime::kUninitializedTerm;
    }

    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(isV1ElectionProtocol),
                              0,
                              rbid,
                              true,
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              stdx::ref(shutdownState),
                              defaultBatchSize);
    ASSERT_EQUALS(OplogFetcher::State::kPreStart, oplogFetcher.getState_forTest());

    ASSERT_OK(oplogFetcher.startup());
    ASSERT_EQUALS(OplogFetcher::State::kRunning, oplogFetcher.getState_forTest());

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.opTime.getTerm()}, 200);

    auto metadataObj = makeOplogQueryMetadataObject(remoteNewerOpTime, rbid, 2, 2);
    processNetworkResponse(
        {makeCursorResponse(cursorId, {firstEntry, secondEntry}), metadataObj, Milliseconds(0)},
        true);

    ASSERT_EQUALS(1U, lastEnqueuedDocuments.size());
    ASSERT_BSONOBJ_EQ(secondEntry, lastEnqueuedDocuments[0]);

    // Set cursor ID to 0 in getMore response to indicate no more data available.
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.opTime.getTerm()}, 300);
    auto fourthEntry = makeNoopOplogEntry({{Seconds(1200), 0}, lastFetched.opTime.getTerm()}, 300);
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
    auto request = testTwoBatchHandling(true);
    ASSERT_EQUALS(dataReplicatorExternalState->currentTerm, request.cmdObj["term"].numberLong());
    ASSERT_EQUALS(dataReplicatorExternalState->lastCommittedOpTime,
                  unittest::assertGet(OpTime::parseFromOplogEntry(
                      request.cmdObj["lastKnownCommittedOpTime"].Obj())));
}

TEST_F(OplogFetcherTest,
       GetMoreRequestUnderProtocolVersionZeroDoesNotIncludeTermOrLastKnownCommittedOpTime) {
    auto request = testTwoBatchHandling(false);
    ASSERT_FALSE(request.cmdObj.hasField("term"));
    ASSERT_FALSE(request.cmdObj.hasField("lastKnownCommittedOpTime"));
}

TEST_F(OplogFetcherTest, ValidateDocumentsReturnsNoSuchKeyIfTimestampIsNotFoundInAnyDocument) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123), 100);
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
    auto firstEntry = makeNoopOplogEntry(Seconds(123), 100);
    auto secondEntry = makeNoopOplogEntry(Seconds(456), 200);

    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  OplogFetcher::validateDocuments(
                      {firstEntry, secondEntry},
                      false,
                      unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp())
                      .getStatus());
}

TEST_F(OplogFetcherTest, ValidateDocumentsReturnsOutOfOrderIfTimestampInSecondEntryIsBeforeFirst) {
    auto firstEntry = makeNoopOplogEntry(Seconds(456), 100);
    auto secondEntry = makeNoopOplogEntry(Seconds(123), 200);

    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  OplogFetcher::validateDocuments(
                      {firstEntry, secondEntry},
                      true,
                      unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp())
                      .getStatus());
}

TEST_F(OplogFetcherTest, ValidateDocumentsReturnsOutOfOrderIfTimestampInThirdEntryIsBeforeSecond) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123), 100);
    auto secondEntry = makeNoopOplogEntry(Seconds(789), 200);
    auto thirdEntry = makeNoopOplogEntry(Seconds(456), 300);

    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  OplogFetcher::validateDocuments(
                      {firstEntry, secondEntry, thirdEntry},
                      true,
                      unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp())
                      .getStatus());
}

TEST_F(OplogFetcherTest,
       ValidateDocumentsExcludesFirstDocumentInApplyCountAndBytesIfProcessingFirstBatch) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123), 100);
    auto secondEntry = makeNoopOplogEntry(Seconds(456), 200);
    auto thirdEntry = makeNoopOplogEntry(Seconds(789), 300);

    auto info = unittest::assertGet(OplogFetcher::validateDocuments(
        {firstEntry, secondEntry, thirdEntry},
        true,
        unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp()));

    ASSERT_EQUALS(3U, info.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  info.networkDocumentBytes);

    ASSERT_EQUALS(300LL, info.lastDocument.value);
    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)),
                  info.lastDocument.opTime);
}

TEST_F(OplogFetcherTest,
       ValidateDocumentsIncludesFirstDocumentInApplyCountAndBytesIfNotProcessingFirstBatch) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123), 100);
    auto secondEntry = makeNoopOplogEntry(Seconds(456), 200);
    auto thirdEntry = makeNoopOplogEntry(Seconds(789), 300);

    auto info = unittest::assertGet(OplogFetcher::validateDocuments(
        {firstEntry, secondEntry, thirdEntry}, false, Timestamp(Seconds(100), 0)));

    ASSERT_EQUALS(3U, info.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  info.networkDocumentBytes);

    ASSERT_EQUALS(info.networkDocumentCount, info.toApplyDocumentCount);
    ASSERT_EQUALS(info.networkDocumentBytes, info.toApplyDocumentBytes);

    ASSERT_EQUALS(300LL, info.lastDocument.value);
    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)),
                  info.lastDocument.opTime);
}

TEST_F(OplogFetcherTest,
       ValidateDocumentsReturnsDefaultLastDocumentHashAndOpTimeWhenThereAreNoDocumentsToApply) {
    auto firstEntry = makeNoopOplogEntry(Seconds(123), 100);

    auto info = unittest::assertGet(OplogFetcher::validateDocuments(
        {firstEntry},
        true,
        unittest::assertGet(OpTime::parseFromOplogEntry(firstEntry)).getTimestamp()));

    ASSERT_EQUALS(1U, info.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize()), info.networkDocumentBytes);

    ASSERT_EQUALS(0U, info.toApplyDocumentCount);
    ASSERT_EQUALS(0U, info.toApplyDocumentBytes);

    ASSERT_EQUALS(0LL, info.lastDocument.value);
    ASSERT_EQUALS(OpTime(), info.lastDocument.opTime);
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

    ASSERT_EQUALS(0LL, info.lastDocument.value);
    ASSERT_EQUALS(OpTime(), info.lastDocument.opTime);
}
}  // namespace
