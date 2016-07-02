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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/data_replicator_external_state_mock.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

class ShutdownState {
    MONGO_DISALLOW_COPYING(ShutdownState);

public:
    ShutdownState();

    Status getStatus() const;
    OpTimeWithHash getLastFetched() const;

    /**
     * Use this for oplog fetcher shutdown callback.
     */
    void operator()(const Status& status, const OpTimeWithHash& lastFetched);

private:
    Status _status = executor::TaskExecutorTest::getDetectableErrorStatus();
    OpTimeWithHash _lastFetched = {0, OpTime()};
};

class OplogFetcherTest : public executor::ThreadPoolExecutorTest {
protected:
    void setUp() override;
    void tearDown() override;

    /**
     * Schedules network response and instructs network interface to process response.
     * Returns remote command request in network request.
     */
    RemoteCommandRequest processNetworkResponse(RemoteCommandResponse response,
                                                bool expectReadyRequestsAfterProcessing = false);
    RemoteCommandRequest processNetworkResponse(BSONObj obj,
                                                bool expectReadyRequestsAfterProcessing = false);

    /**
     * Starts an oplog fetcher. Processes a single batch of results from
     * the oplog query and shuts down.
     * Returns shutdown state.
     */
    std::unique_ptr<ShutdownState> processSingleBatch(RemoteCommandResponse response);
    std::unique_ptr<ShutdownState> processSingleBatch(BSONObj obj);

    /**
     * Tests checkSyncSource result handling.
     */
    void testSyncSourceChecking(rpc::ReplSetMetadata* metadata);

    /**
     * Tests handling of two batches of operations returned from query.
     * Returns getMore request.
     */
    RemoteCommandRequest testTwoBatchHandling(bool isV1ElectionProtocol);

    OpTimeWithHash lastFetched;

    std::unique_ptr<DataReplicatorExternalStateMock> dataReplicatorExternalState;

    Fetcher::Documents lastEnqueuedDocuments;
    OplogFetcher::DocumentsInfo lastEnqueuedDocumentsInfo;
    Milliseconds lastEnqueuedElapsed;
    OplogFetcher::EnqueueDocumentsFn enqueueDocumentsFn;
};

ShutdownState::ShutdownState() = default;

Status ShutdownState::getStatus() const {
    return _status;
}

OpTimeWithHash ShutdownState::getLastFetched() const {
    return _lastFetched;
}

void ShutdownState::operator()(const Status& status, const OpTimeWithHash& lastFetched) {
    _status = status;
    _lastFetched = lastFetched;
}

void OplogFetcherTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    launchExecutorThread();

    lastFetched = {456LL, {{123, 0}, 1}};

    dataReplicatorExternalState = stdx::make_unique<DataReplicatorExternalStateMock>();
    dataReplicatorExternalState->currentTerm = lastFetched.opTime.getTerm();
    dataReplicatorExternalState->lastCommittedOpTime = {{9999, 0}, lastFetched.opTime.getTerm()};

    enqueueDocumentsFn = [this](Fetcher::Documents::const_iterator begin,
                                Fetcher::Documents::const_iterator end,
                                const OplogFetcher::DocumentsInfo& info,
                                Milliseconds elapsed) {
        lastEnqueuedDocuments = {begin, end};
        lastEnqueuedDocumentsInfo = info;
        lastEnqueuedElapsed = elapsed;
    };
}

void OplogFetcherTest::tearDown() {
    executor::ThreadPoolExecutorTest::tearDown();
}

RemoteCommandRequest OplogFetcherTest::processNetworkResponse(
    RemoteCommandResponse response, bool expectReadyRequestsAfterProcessing) {
    auto net = getNet();
    net->enterNetwork();
    auto request = net->scheduleSuccessfulResponse(response);
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(expectReadyRequestsAfterProcessing, net->hasReadyRequests());
    net->exitNetwork();
    return request;
}

RemoteCommandRequest OplogFetcherTest::processNetworkResponse(
    BSONObj obj, bool expectReadyRequestsAfterProcessing) {
    return processNetworkResponse({obj, rpc::makeEmptyMetadata(), Milliseconds(0)},
                                  expectReadyRequestsAfterProcessing);
}

HostAndPort source("localhost:12345");
NamespaceString nss("local.oplog.rs");

ReplicaSetConfig _createConfig(bool isV1ElectionProtocol) {
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

    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(configObj));
    return config;
}

std::unique_ptr<ShutdownState> OplogFetcherTest::processSingleBatch(
    RemoteCommandResponse response) {
    auto shutdownState = stdx::make_unique<ShutdownState>();

    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(true),
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              stdx::ref(*shutdownState));

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    auto request = processNetworkResponse(response);

    ASSERT_EQUALS(oplogFetcher.getCommandObject_forTest(), request.cmdObj);
    ASSERT_EQUALS(oplogFetcher.getMetadataObject_forTest(), request.metadata);

    oplogFetcher.shutdown();
    oplogFetcher.join();

    return shutdownState;
}

std::unique_ptr<ShutdownState> OplogFetcherTest::processSingleBatch(BSONObj obj) {
    return processSingleBatch({obj, rpc::makeEmptyMetadata(), Milliseconds(0)});
}

TEST_F(OplogFetcherTest, InvalidConstruction) {
    // Null start timestamp.
    ASSERT_THROWS_CODE_AND_WHAT(OplogFetcher(&getExecutor(),
                                             OpTimeWithHash(),
                                             source,
                                             nss,
                                             _createConfig(true),
                                             dataReplicatorExternalState.get(),
                                             enqueueDocumentsFn,
                                             [](Status, OpTimeWithHash) {}),
                                UserException,
                                ErrorCodes::BadValue,
                                "null last optime fetched");

    // Null EnqueueDocumentsFn.
    ASSERT_THROWS_CODE_AND_WHAT(OplogFetcher(&getExecutor(),
                                             lastFetched,
                                             source,
                                             nss,
                                             _createConfig(true),
                                             dataReplicatorExternalState.get(),
                                             OplogFetcher::EnqueueDocumentsFn(),
                                             [](Status, OpTimeWithHash) {}),
                                UserException,
                                ErrorCodes::BadValue,
                                "null enqueueDocuments function");

    // Uninitialized replica set configuration.
    ASSERT_THROWS_CODE_AND_WHAT(OplogFetcher(&getExecutor(),
                                             lastFetched,
                                             source,
                                             nss,
                                             ReplicaSetConfig(),
                                             dataReplicatorExternalState.get(),
                                             enqueueDocumentsFn,
                                             [](Status, OpTimeWithHash) {}),
                                UserException,
                                ErrorCodes::InvalidReplicaSetConfig,
                                "uninitialized replica set configuration");

    // Null OnShutdownCallbackFn.
    ASSERT_THROWS_CODE_AND_WHAT(OplogFetcher(&getExecutor(),
                                             lastFetched,
                                             source,
                                             nss,
                                             _createConfig(true),
                                             dataReplicatorExternalState.get(),
                                             enqueueDocumentsFn,
                                             OplogFetcher::OnShutdownCallbackFn()),
                                UserException,
                                ErrorCodes::BadValue,
                                "null onShutdownCallback function");
}

void _checkDefaultCommandObjectFields(BSONObj cmdObj) {
    ASSERT_EQUALS(std::string("find"), cmdObj.firstElementFieldName());
    ASSERT_TRUE(cmdObj.getBoolField("tailable"));
    ASSERT_TRUE(cmdObj.getBoolField("oplogReplay"));
    ASSERT_TRUE(cmdObj.getBoolField("awaitData"));
    ASSERT_EQUALS(60000, cmdObj.getIntField("maxTimeMS"));
}

TEST_F(
    OplogFetcherTest,
    CommandObjectContainsTermAndStartTimestampIfGetCurrentTermAndLastCommittedOpTimeReturnsValidTerm) {
    auto cmdObj = OplogFetcher(&getExecutor(),
                               lastFetched,
                               source,
                               nss,
                               _createConfig(true),
                               dataReplicatorExternalState.get(),
                               enqueueDocumentsFn,
                               [](Status, OpTimeWithHash) {})
                      .getCommandObject_forTest();
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["filter"].type());
    ASSERT_EQUALS(BSON("ts" << BSON("$gte" << lastFetched.opTime.getTimestamp())),
                  cmdObj["filter"].Obj());
    ASSERT_EQUALS(dataReplicatorExternalState->currentTerm, cmdObj["term"].numberLong());
    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(
    OplogFetcherTest,
    CommandObjectContainsDoesNotContainTermIfGetCurrentTermAndLastCommittedOpTimeReturnsUninitializedTerm) {
    dataReplicatorExternalState->currentTerm = OpTime::kUninitializedTerm;
    auto cmdObj = OplogFetcher(&getExecutor(),
                               lastFetched,
                               source,
                               nss,
                               _createConfig(true),
                               dataReplicatorExternalState.get(),
                               enqueueDocumentsFn,
                               [](Status, OpTimeWithHash) {})
                      .getCommandObject_forTest();
    ASSERT_EQUALS(mongo::BSONType::Object, cmdObj["filter"].type());
    ASSERT_EQUALS(BSON("ts" << BSON("$gte" << lastFetched.opTime.getTimestamp())),
                  cmdObj["filter"].Obj());
    ASSERT_FALSE(cmdObj.hasField("term"));
    _checkDefaultCommandObjectFields(cmdObj);
}

TEST_F(OplogFetcherTest, MetadataObjectContainsReplSetMetadataFieldUnderProtocolVersion1) {
    auto metadataObj = OplogFetcher(&getExecutor(),
                                    lastFetched,
                                    source,
                                    nss,
                                    _createConfig(true),
                                    dataReplicatorExternalState.get(),
                                    enqueueDocumentsFn,
                                    [](Status, OpTimeWithHash) {})
                           .getMetadataObject_forTest();
    ASSERT_EQUALS(2, metadataObj.nFields());
    ASSERT_EQUALS(1, metadataObj[rpc::kReplSetMetadataFieldName].numberInt());
}

TEST_F(OplogFetcherTest, MetadataObjectIsEmptyUnderProtocolVersion0) {
    auto metadataObj = OplogFetcher(&getExecutor(),
                                    lastFetched,
                                    source,
                                    nss,
                                    _createConfig(false),
                                    dataReplicatorExternalState.get(),
                                    enqueueDocumentsFn,
                                    [](Status, OpTimeWithHash) {})
                           .getMetadataObject_forTest();
    ASSERT_EQUALS(BSON(rpc::ServerSelectionMetadata::fieldName()
                       << BSON(rpc::ServerSelectionMetadata::kSecondaryOkFieldName << 1)),
                  metadataObj);
}

TEST_F(OplogFetcherTest, RemoteCommandTimeoutShouldEqualElectionTimeout) {
    auto config = _createConfig(true);
    auto timeout = OplogFetcher(&getExecutor(),
                                lastFetched,
                                source,
                                nss,
                                config,
                                dataReplicatorExternalState.get(),
                                enqueueDocumentsFn,
                                [](Status, OpTimeWithHash) {})
                       .getRemoteCommandTimeout_forTest();
    ASSERT_EQUALS(config.getElectionTimeoutPeriod(), timeout);
}

TEST_F(OplogFetcherTest, AwaitDataTimeoutShouldEqualHalfElectionTimeoutUnderProtocolVersion1) {
    auto config = _createConfig(true);
    auto timeout = OplogFetcher(&getExecutor(),
                                lastFetched,
                                source,
                                nss,
                                config,
                                dataReplicatorExternalState.get(),
                                enqueueDocumentsFn,
                                [](Status, OpTimeWithHash) {})
                       .getAwaitDataTimeout_forTest();
    ASSERT_EQUALS(config.getElectionTimeoutPeriod() / 2, timeout);
}

TEST_F(OplogFetcherTest, AwaitDataTimeoutShouldBeAConstantUnderProtocolVersion0) {
    auto timeout = OplogFetcher(&getExecutor(),
                                lastFetched,
                                source,
                                nss,
                                _createConfig(false),
                                dataReplicatorExternalState.get(),
                                enqueueDocumentsFn,
                                [](Status, OpTimeWithHash) {})
                       .getAwaitDataTimeout_forTest();
    ASSERT_EQUALS(OplogFetcher::kDefaultProtocolZeroAwaitDataTimeout, timeout);
}

TEST_F(OplogFetcherTest, ShuttingExecutorDownShouldPreventOplogFetcherFromStarting) {
    getExecutor().shutdown();

    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(true),
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              [](Status, OpTimeWithHash) {});

    // Last optime and hash fetched should match values passed to constructor.
    ASSERT_EQUALS(lastFetched, oplogFetcher.getLastOpTimeWithHashFetched());

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, oplogFetcher.startup());
    ASSERT_FALSE(oplogFetcher.isActive());

    // Last optime and hash fetched should not change.
    ASSERT_EQUALS(lastFetched, oplogFetcher.getLastOpTimeWithHashFetched());
}

TEST_F(OplogFetcherTest, ShuttingExecutorDownAfterStartupStopsTheOplogFetcher) {
    ShutdownState shutdownState;

    OplogFetcher oplogFetcher(&getExecutor(),
                              lastFetched,
                              source,
                              nss,
                              _createConfig(true),
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              stdx::ref(shutdownState));

    ASSERT_FALSE(oplogFetcher.isActive());
    ASSERT_OK(oplogFetcher.startup());
    ASSERT_TRUE(oplogFetcher.isActive());

    getExecutor().shutdown();

    oplogFetcher.join();

    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, shutdownState.getStatus());
    ASSERT_EQUALS(lastFetched, shutdownState.getLastFetched());
}

BSONObj makeNoopOplogEntry(OpTimeWithHash opTimeWithHash) {
    BSONObjBuilder bob;
    bob.appendElements(opTimeWithHash.opTime.toBSON());
    bob.append("h", opTimeWithHash.value);
    bob.append("op", "c");
    bob.append("ns", "test.t");
    return bob.obj();
}

BSONObj makeNoopOplogEntry(OpTime opTime, long long hash) {
    return makeNoopOplogEntry({hash, opTime});
}

BSONObj makeNoopOplogEntry(Seconds seconds, long long hash) {
    return makeNoopOplogEntry({{seconds, 0}, 1LL}, hash);
}

BSONObj makeCursorResponse(CursorId cursorId,
                           Fetcher::Documents oplogEntries,
                           bool isFirstBatch = true) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
        cursorBob.append("id", cursorId);
        cursorBob.append("ns", nss.toString());
        {
            BSONArrayBuilder batchBob(
                cursorBob.subarrayStart(isFirstBatch ? "firstBatch" : "nextBatch"));
            for (auto oplogEntry : oplogEntries) {
                batchBob.append(oplogEntry);
            }
        }
    }
    bob.append("ok", 1);
    return bob.obj();
}

TEST_F(OplogFetcherTest, InvalidMetadataInResponseStopsTheOplogFetcher) {
    auto shutdownState = processSingleBatch(
        {makeCursorResponse(0, {}),
         BSON(rpc::kReplSetMetadataFieldName << BSON("invalid_repl_metadata_field" << 1)),
         Milliseconds(0)});

    ASSERT_EQUALS(ErrorCodes::NoSuchKey, shutdownState->getStatus());
}

TEST_F(OplogFetcherTest, VaidMetadataInResponseShouldBeForwardedToProcessMetadataFn) {
    rpc::ReplSetMetadata metadata(1, lastFetched.opTime, lastFetched.opTime, 1, OID::gen(), 2, 2);
    BSONObjBuilder bob;
    ASSERT_OK(metadata.writeToMetadata(&bob));
    auto metadataObj = bob.obj();
    processSingleBatch(
        {makeCursorResponse(0, {makeNoopOplogEntry(lastFetched)}), metadataObj, Milliseconds(0)});
    ASSERT_EQUALS(metadata.getPrimaryIndex(),
                  dataReplicatorExternalState->metadataProcessed.getPrimaryIndex());
}

TEST_F(OplogFetcherTest, EmptyFirstBatchStopsOplogFetcherWithRemoteOplogStaleError) {
    ASSERT_EQUALS(ErrorCodes::RemoteOplogStale,
                  processSingleBatch(makeCursorResponse(0, {}))->getStatus());
}

TEST_F(OplogFetcherTest,
       MissingOpTimeInFirstDocumentCausesOplogFetcherToStopWithOplogStartMissingError) {
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch(makeCursorResponse(0, {BSONObj()}))->getStatus());
}

TEST_F(
    OplogFetcherTest,
    LastOpTimeFetchedDoesNotMatchFirstDocumentCausesOplogFetcherToStopWithOplogStartMissingError) {
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  processSingleBatch(
                      makeCursorResponse(0, {makeNoopOplogEntry(Seconds(456), lastFetched.value)}))
                      ->getStatus());
}

TEST_F(OplogFetcherTest,
       LastHashFetchedDoesNotMatchFirstDocumentCausesOplogFetcherToStopWithOplogStartMissingError) {
    ASSERT_EQUALS(
        ErrorCodes::OplogStartMissing,
        processSingleBatch(
            makeCursorResponse(0, {makeNoopOplogEntry(lastFetched.opTime, lastFetched.value + 1)}))
            ->getStatus());
}

TEST_F(OplogFetcherTest,
       MissingOpTimeInSecondDocumentOfFirstBatchCausesOplogFetcherToStopWithNoSuchKey) {
    ASSERT_EQUALS(
        ErrorCodes::NoSuchKey,
        processSingleBatch(makeCursorResponse(0,
                                              {makeNoopOplogEntry(lastFetched),
                                               BSON("o" << BSON("msg"
                                                                << "oplog entry without optime"))}))
            ->getStatus());
}

TEST_F(OplogFetcherTest, TimestampsNotAdvancingInBatchCausesOplogFetcherStopWithOplogOutOfOrder) {
    ASSERT_EQUALS(ErrorCodes::OplogOutOfOrder,
                  processSingleBatch(makeCursorResponse(0,
                                                        {makeNoopOplogEntry(lastFetched),
                                                         makeNoopOplogEntry(Seconds(1000), 1),
                                                         makeNoopOplogEntry(Seconds(2000), 1),
                                                         makeNoopOplogEntry(Seconds(1500), 1)}))
                      ->getStatus());
}

TEST_F(OplogFetcherTest, OplogFetcherShouldExcludeFirstDocumentInFirstBatchWhenEnqueuingDocuments) {
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.opTime.getTerm()}, 200);
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.opTime.getTerm()}, 300);
    Fetcher::Documents documents{firstEntry, secondEntry, thirdEntry};

    Milliseconds elapsed(600);
    auto shutdownState =
        processSingleBatch({makeCursorResponse(0, documents), rpc::makeEmptyMetadata(), elapsed});

    ASSERT_EQUALS(2U, lastEnqueuedDocuments.size());
    ASSERT_EQUALS(secondEntry, lastEnqueuedDocuments[0]);
    ASSERT_EQUALS(thirdEntry, lastEnqueuedDocuments[1]);

    ASSERT_EQUALS(3U, lastEnqueuedDocumentsInfo.networkDocumentCount);
    ASSERT_EQUALS(size_t(firstEntry.objsize() + secondEntry.objsize() + thirdEntry.objsize()),
                  lastEnqueuedDocumentsInfo.networkDocumentBytes);

    ASSERT_EQUALS(2U, lastEnqueuedDocumentsInfo.toApplyDocumentCount);
    ASSERT_EQUALS(size_t(secondEntry.objsize() + thirdEntry.objsize()),
                  lastEnqueuedDocumentsInfo.toApplyDocumentBytes);

    ASSERT_EQUALS(thirdEntry["h"].numberLong(), lastEnqueuedDocumentsInfo.lastDocument.value);
    ASSERT_EQUALS(unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry)),
                  lastEnqueuedDocumentsInfo.lastDocument.opTime);

    ASSERT_EQUALS(elapsed, lastEnqueuedElapsed);

    // The last fetched optime and hash should be updated after pushing the operations into the
    // buffer and reflected in the shutdown callback arguments.
    ASSERT_OK(shutdownState->getStatus());
    ASSERT_EQUALS(OpTimeWithHash(thirdEntry["h"].numberLong(),
                                 unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry))),
                  shutdownState->getLastFetched());
}

void OplogFetcherTest::testSyncSourceChecking(rpc::ReplSetMetadata* metadata) {
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.opTime.getTerm()}, 200);
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.opTime.getTerm()}, 300);
    Fetcher::Documents documents{firstEntry, secondEntry, thirdEntry};

    BSONObj metadataObj;
    if (metadata) {
        BSONObjBuilder bob;
        ASSERT_OK(metadata->writeToMetadata(&bob));
        metadataObj = bob.obj();
    }

    dataReplicatorExternalState->shouldStopFetchingResult = true;

    auto shutdownState =
        processSingleBatch({makeCursorResponse(0, documents), metadataObj, Milliseconds(0)});

    // Sync source checking happens after we have successfully pushed the operations into
    // the buffer for the next replication phase (eg. applier).
    // The last fetched optime and hash should be reflected in the shutdown callback
    // arguments.
    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, shutdownState->getStatus());
    ASSERT_EQUALS(OpTimeWithHash(thirdEntry["h"].numberLong(),
                                 unittest::assertGet(OpTime::parseFromOplogEntry(thirdEntry))),
                  shutdownState->getLastFetched());
}

TEST_F(OplogFetcherTest, FailedSyncSourceCheckWithoutMetadataStopsTheOplogFetcher) {
    testSyncSourceChecking(nullptr);

    // Sync source optime and "hasSyncSource" are not available if the respone does not
    // contain metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(OpTime(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_FALSE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

TEST_F(OplogFetcherTest, FailedSyncSourceCheckWithMetadataStopsTheOplogFetcher) {
    rpc::ReplSetMetadata metadata(lastFetched.opTime.getTerm(),
                                  {{Seconds(10000), 0}, 1},
                                  {{Seconds(20000), 0}, 1},
                                  1,
                                  OID::gen(),
                                  2,
                                  2);

    testSyncSourceChecking(&metadata);

    // Sync source optime and "hasSyncSource" can be set if the respone contains metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(metadata.getLastOpVisible(), dataReplicatorExternalState->syncSourceLastOpTime);
    ASSERT_TRUE(dataReplicatorExternalState->syncSourceHasSyncSource);
}

TEST_F(OplogFetcherTest,
       FailedSyncSourceCheckWithSyncSourceHavingNoSyncSourceStopsTheOplogFetcher) {
    rpc::ReplSetMetadata metadata(lastFetched.opTime.getTerm(),
                                  {{Seconds(10000), 0}, 1},
                                  {{Seconds(20000), 0}, 1},
                                  1,
                                  OID::gen(),
                                  2,
                                  -1);

    testSyncSourceChecking(&metadata);

    // Sync source "hasSyncSource" is derived from metadata.
    ASSERT_EQUALS(source, dataReplicatorExternalState->lastSyncSourceChecked);
    ASSERT_EQUALS(metadata.getLastOpVisible(), dataReplicatorExternalState->syncSourceLastOpTime);
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
                              dataReplicatorExternalState.get(),
                              enqueueDocumentsFn,
                              stdx::ref(shutdownState));

    ASSERT_OK(oplogFetcher.startup());

    CursorId cursorId = 22LL;
    auto firstEntry = makeNoopOplogEntry(lastFetched);
    auto secondEntry = makeNoopOplogEntry({{Seconds(456), 0}, lastFetched.opTime.getTerm()}, 200);
    processNetworkResponse(makeCursorResponse(cursorId, {firstEntry, secondEntry}), true);

    ASSERT_EQUALS(1U, lastEnqueuedDocuments.size());
    ASSERT_EQUALS(secondEntry, lastEnqueuedDocuments[0]);

    // Set cursor ID to 0 in getMore response to indicate no more data available.
    auto thirdEntry = makeNoopOplogEntry({{Seconds(789), 0}, lastFetched.opTime.getTerm()}, 300);
    auto fourthEntry = makeNoopOplogEntry({{Seconds(1200), 0}, lastFetched.opTime.getTerm()}, 300);
    auto request = processNetworkResponse(makeCursorResponse(0, {thirdEntry, fourthEntry}, false));

    ASSERT_EQUALS(std::string("getMore"), request.cmdObj.firstElementFieldName());
    ASSERT_EQUALS(nss.coll(), request.cmdObj["collection"].String());
    ASSERT_EQUALS(int(durationCount<Milliseconds>(oplogFetcher.getAwaitDataTimeout_forTest())),
                  request.cmdObj.getIntField("maxTimeMS"));

    ASSERT_EQUALS(2U, lastEnqueuedDocuments.size());
    ASSERT_EQUALS(thirdEntry, lastEnqueuedDocuments[0]);
    ASSERT_EQUALS(fourthEntry, lastEnqueuedDocuments[1]);

    oplogFetcher.shutdown();
    oplogFetcher.join();

    ASSERT_OK(shutdownState.getStatus());
    ASSERT_EQUALS(OpTimeWithHash(fourthEntry["h"].numberLong(),
                                 unittest::assertGet(OpTime::parseFromOplogEntry(fourthEntry))),
                  shutdownState.getLastFetched());

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
