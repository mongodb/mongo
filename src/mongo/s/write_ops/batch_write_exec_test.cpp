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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/vector_clock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

const HostAndPort kTestShardHost1 = HostAndPort("FakeHost1", 12345);
const std::string kShardName1 = "FakeShard1";
const HostAndPort kTestShardHost2 = HostAndPort("FakeHost2", 12345);
const std::string kShardName2 = "FakeShard2";

const int kMaxRoundsWithoutProgress = 5;

BSONObj expectInsertsReturnStaleVersionErrorsBase(const NamespaceString& nss,
                                                  const std::vector<BSONObj>& expected,
                                                  const executor::RemoteCommandRequest& request) {
    ASSERT_EQUALS(nss.db(), request.dbname);

    const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
    const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
    ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

    const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
    ASSERT_EQUALS(expected.size(), inserted.size());

    auto itInserted = inserted.begin();
    auto itExpected = expected.begin();

    for (; itInserted != inserted.end(); itInserted++, itExpected++) {
        ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
    }

    BatchedCommandResponse staleResponse;
    staleResponse.setStatus(Status::OK());
    staleResponse.setN(0);

    auto epoch = OID::gen();
    Timestamp timestamp(1);

    // Report a stale version error for each write in the batch.
    int i = 0;
    for (itInserted = inserted.begin(); itInserted != inserted.end(); ++itInserted) {
        staleResponse.addToErrDetails(write_ops::WriteError(
            i,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {1, 0}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {2, 0}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName1)),
                   "Stale error")));
        ++i;
    }

    return staleResponse.toBSON();
}

BSONObj expectInsertsReturnStaleDbVersionErrorsBase(const NamespaceString& nss,
                                                    const std::vector<BSONObj>& expected,
                                                    const executor::RemoteCommandRequest& request) {
    ASSERT_EQUALS(nss.db(), request.dbname);

    const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
    const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
    ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

    const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
    ASSERT_EQUALS(expected.size(), inserted.size());

    auto itInserted = inserted.begin();
    auto itExpected = expected.begin();

    for (; itInserted != inserted.end(); itInserted++, itExpected++) {
        ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
    }

    BSONObjBuilder staleResponse;
    staleResponse.append("ok", 1);
    staleResponse.append("n", 0);

    // Report a stale db version error for each write in the batch.
    int i = 0;
    std::vector<BSONObj> errors;
    for (itInserted = inserted.begin(); itInserted != inserted.end(); ++itInserted) {
        BSONObjBuilder errorBuilder;
        errorBuilder.append("index", i);
        errorBuilder.append("code", int(ErrorCodes::StaleDbVersion));

        auto dbVersion = DatabaseVersion(UUID::gen(), Timestamp(1, 1));
        errorBuilder.append("db", nss.db());
        errorBuilder.append("vReceived", dbVersion.toBSON());
        errorBuilder.append("vWanted", dbVersion.makeUpdated().toBSON());

        errorBuilder.append("errmsg", "mock stale db version");

        errors.push_back(errorBuilder.obj());
        ++i;
    }
    staleResponse.append("writeErrors", errors);

    return staleResponse.obj();
}

/**
 * Expects to send tenantMigrationAborted error for the numberOfFailedOps given.
 * If
 */
BSONObj expectInsertsReturnTenantMigrationAbortedErrorsBase(
    const NamespaceString& nss,
    const std::vector<BSONObj>& expected,
    const executor::RemoteCommandRequest& request,
    int numberOfFailedOps) {
    ASSERT_EQUALS(nss.db(), request.dbname);

    const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
    const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
    ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

    const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
    ASSERT_EQUALS(expected.size(), inserted.size());

    auto itInserted = inserted.begin();
    auto itExpected = expected.begin();

    for (; itInserted != inserted.end(); itInserted++, itExpected++) {
        ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
    }

    BSONObjBuilder tenantMigrationAbortedResponse;
    tenantMigrationAbortedResponse.append("ok", 1);
    tenantMigrationAbortedResponse.append("n", int(inserted.size()));

    int expectedSize = int(expected.size());
    invariant(numberOfFailedOps >= 0 && numberOfFailedOps <= expectedSize,
              str::stream() << "Expected numberOfFailedOps value to be between 0 and "
                            << expectedSize << " but found " << numberOfFailedOps << ".");
    int i = expectedSize - numberOfFailedOps;

    std::vector<BSONObj> errors;
    for (; i < expectedSize; i++) {
        BSONObjBuilder errorBuilder;
        errorBuilder.append("index", i);
        errorBuilder.append("code", int(ErrorCodes::TenantMigrationAborted));
        errorBuilder.append("errmsg", "mock tenantmigrationaborted error");
        errors.push_back(errorBuilder.obj());
        // ordered bulk only return one error and stop.
        if (actualBatchedInsert.getWriteCommandRequestBase().getOrdered())
            break;
    }
    tenantMigrationAbortedResponse.append("writeErrors", errors);

    return tenantMigrationAbortedResponse.obj();
}

/**
 * Mimics a single shard backend for a particular collection which can be initialized with a
 * set of write command results to return.
 */
class BatchWriteExecTest : public ShardingTestFixture {
public:
    BatchWriteExecTest() = default;
    ~BatchWriteExecTest() = default;

    void setUp() override {
        ShardingTestFixture::setUp();
        setRemote(HostAndPort("ClientHost", 12345));

        // Set up the RemoteCommandTargeter for the config shard
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost1), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost1));
            targeter->setFindHostReturnValue(kTestShardHost1);
            return targeter;
        }());

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost2), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost2));
            targeter->setFindHostReturnValue(kTestShardHost2);
            return targeter;
        }());

        // Set up the shard registry to contain the fake shards
        setupShards({[] {
                         ShardType shardType;
                         shardType.setName(kShardName1);
                         shardType.setHost(kTestShardHost1.toString());
                         return shardType;
                     }(),
                     [] {
                         ShardType shardType;
                         shardType.setName(kShardName2);
                         shardType.setHost(kTestShardHost2.toString());
                         return shardType;
                     }()});
    }

    void expectInsertsReturnSuccess(const std::vector<BSONObj>& expected) {
        expectInsertsReturnSuccess(expected.begin(), expected.end());
    }

    void expectInsertsReturnSuccess(std::vector<BSONObj>::const_iterator expectedFrom,
                                    std::vector<BSONObj>::const_iterator expectedTo) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS(nss.db(), request.dbname);

            const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
            const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
            ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

            const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
            const size_t expectedSize = std::distance(expectedFrom, expectedTo);
            ASSERT_EQUALS(expectedSize, inserted.size());

            auto itInserted = inserted.begin();
            auto itExpected = expectedFrom;

            for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
            }

            BatchedCommandResponse response;
            response.setStatus(Status::OK());
            response.setN(inserted.size());

            return response.toBSON();
        });
    }

    void expectInsertsReturnStaleVersionErrors(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return expectInsertsReturnStaleVersionErrorsBase(nss, expected, request);
        });
    }

    void expectInsertsReturnStaleDbVersionErrors(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return expectInsertsReturnStaleDbVersionErrorsBase(nss, expected, request);
        });
    }

    void expectInsertsReturnTenantMigrationAbortedErrors(const std::vector<BSONObj>& expected,
                                                         int numberOfFailedOps) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return expectInsertsReturnTenantMigrationAbortedErrorsBase(
                nss, expected, request, numberOfFailedOps);
        });
    }

    void expectInsertsReturnError(const std::vector<BSONObj>& expected,
                                  const BatchedCommandResponse& errResponse) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            try {
                ASSERT_EQUALS(nss.db(), request.dbname);

                const auto opMsgRequest(
                    OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
                const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
                ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

                const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
                ASSERT_EQUALS(expected.size(), inserted.size());

                auto itInserted = inserted.begin();
                auto itExpected = expected.begin();

                for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                    ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
                }

                return errResponse.toBSON();
            } catch (const DBException& ex) {
                BSONObjBuilder bb;
                CommandHelpers::appendCommandStatusNoThrow(bb, ex.toStatus());
                return bb.obj();
            }
        });
    }

    const NamespaceString nss{"foo.bar"};

    const CollectionGeneration gen{OID::gen(), Timestamp(1, 1)};
    MockNSTargeter singleShardNSTargeter{
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion(gen, {100, 200}),
                                              CollectionIndexes(gen, boost::none)),
                                 boost::none),
                   BSON("x" << MINKEY),
                   BSON("x" << MAXKEY))}};
};

//
// Tests for the BatchWriteExec
//

TEST_F(BatchWriteExecTest, SingleOpUnordered) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Do single-target, single doc batch write op
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(1LL, response.getN());
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnSuccess(std::vector<BSONObj>{BSON("x" << 1)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, SingleUpdateTargetsShardWithLet) {
    // Try to update the single doc where a let param is used in the shard key.
    const auto let = BSON("y" << 100);
    const auto rtc = LegacyRuntimeConstants{Date_t::now(), Timestamp(1, 1)};
    const auto q = BSON("x"
                        << "$$y");
    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setLet(let);
        updateOp.setLegacyRuntimeConstants(rtc);
        updateOp.setUpdates(std::vector{write_ops::UpdateOpEntry(
            q,
            write_ops::UpdateModification::parseFromClassicUpdate(BSON("Key"
                                                                       << "100")))});
        return updateOp;
    }());
    updateRequest.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << MINKEY),
                   BSON("x" << 0)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << 0),
                   BSON("x" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, updateRequest, &response, &stats);

        return response;
    });

    // The update will hit the first shard.
    onCommandForPoolExecutor(
        [&](const RemoteCommandRequest& request) {
            ASSERT_EQ(kTestShardHost2, request.target);

            BatchedCommandResponse response;
            response.setStatus(Status::OK());
            response.setNModified(1);

            // Check that let params and runtimeConstants are propigated to shards.
            const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
            const auto actualBatchedUpdate(BatchedCommandRequest::parseUpdate(opMsgRequest));
            ASSERT_BSONOBJ_EQ(let, actualBatchedUpdate.getLet().value_or(BSONObj()));
            ASSERT_EQUALS(actualBatchedUpdate.getLegacyRuntimeConstants()->getLocalNow(),
                          rtc.getLocalNow());
            ASSERT_EQUALS(actualBatchedUpdate.getLegacyRuntimeConstants()->getClusterTime(),
                          rtc.getClusterTime());

            // Check that let params are only forwarded and not evaluated.
            auto expectedQ = BSON("x"
                                  << "$$y");
            for (auto&& u : actualBatchedUpdate.getUpdateRequest().getUpdates())
                ASSERT_BSONOBJ_EQ(expectedQ, u.getQ());

            return response.toBSON();
        });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(1, response.getNModified());
}

TEST_F(BatchWriteExecTest, SingleDeleteTargetsShardWithLet) {
    // Try to update the single doc where a let param is used in the shard key.
    const auto let = BSON("y" << 100);
    const auto rtc = LegacyRuntimeConstants{Date_t::now(), Timestamp(1, 1)};
    const auto q = BSON("x"
                        << "$$y");
    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        deleteOp.setLet(let);
        deleteOp.setLegacyRuntimeConstants(rtc);
        deleteOp.setDeletes(std::vector{write_ops::DeleteOpEntry(q, false)});
        return deleteOp;
    }());
    deleteRequest.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();

    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

    protected:
        std::vector<ShardEndpoint> targetDelete(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{ShardEndpoint(
                kShardName2,
                ShardVersion(ChunkVersion({epoch, Timestamp(1, 1)}, {101, 200}),
                             CollectionIndexes({epoch, Timestamp(1, 1)}, boost::none)),
                boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(
             ShardEndpoint(kShardName1,
                           ShardVersion(ChunkVersion({epoch, Timestamp(1, 1)}, {100, 200}),
                                        CollectionIndexes({epoch, Timestamp(1, 1)}, boost::none)),
                           boost::none),
             BSON("x" << MINKEY),
             BSON("x" << 0)),
         MockRange(
             ShardEndpoint(kShardName2,
                           ShardVersion(ChunkVersion({epoch, Timestamp(1, 1)}, {101, 200}),
                                        CollectionIndexes({epoch, Timestamp(1, 1)}, boost::none)),
                           boost::none),
             BSON("x" << 0),
             BSON("x" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, deleteRequest, &response, &stats);

        return response;
    });

    // The update will hit the first shard.
    onCommandForPoolExecutor(
        [&](const RemoteCommandRequest& request) {
            ASSERT_EQ(kTestShardHost2, request.target);

            BatchedCommandResponse response;
            response.setStatus(Status::OK());

            // Check that let params are propigated to shards.
            const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
            const auto actualBatchedUpdate(BatchedCommandRequest::parseDelete(opMsgRequest));
            ASSERT_BSONOBJ_EQ(let, actualBatchedUpdate.getLet().value_or(BSONObj()));
            ASSERT_EQUALS(actualBatchedUpdate.getLegacyRuntimeConstants()->getLocalNow(),
                          rtc.getLocalNow());
            ASSERT_EQUALS(actualBatchedUpdate.getLegacyRuntimeConstants()->getClusterTime(),
                          rtc.getClusterTime());

            // Check that let params are only forwarded and not evaluated.
            auto expectedQ = BSON("x"
                                  << "$$y");
            for (auto&& u : actualBatchedUpdate.getDeleteRequest().getDeletes())
                ASSERT_BSONOBJ_EQ(expectedQ, u.getQ());

            return response.toBSON();
        });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
}

TEST_F(BatchWriteExecTest, MultiOpLargeOrdered) {
    const int kNumDocsToInsert = 100'000;
    const std::string kDocValue(200, 'x');

    std::vector<BSONObj> docsToInsert;
    docsToInsert.reserve(kNumDocsToInsert);
    for (int i = 0; i < kNumDocsToInsert; i++) {
        docsToInsert.push_back(BSON("_id" << i << "someLargeKeyToWasteSpace" << kDocValue));
    }

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments(docsToInsert);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQUALS(response.getN(), kNumDocsToInsert);
        ASSERT_EQUALS(stats.numRounds, 2);
    });

    expectInsertsReturnSuccess(docsToInsert.begin(), docsToInsert.begin() + 66576);
    expectInsertsReturnSuccess(docsToInsert.begin() + 66576, docsToInsert.end());

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, SingleOpUnorderedError) {
    BatchedCommandResponse errResponse;
    errResponse.setStatus({ErrorCodes::UnknownError, "mock error"});

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQ(errResponse.toStatus().code(), response.getErrDetailsAt(0).getStatus().code());
        ASSERT(response.getErrDetailsAt(0).getStatus().reason().find(
                   errResponse.toStatus().reason()) != std::string::npos);

        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1)}, errResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, MultiOpLargeUnorderedWithStaleShardVersionError) {
    const int kNumDocsToInsert = 100'000;

    std::vector<BSONObj> docsToInsert;
    docsToInsert.reserve(kNumDocsToInsert);
    for (int i = 0; i < kNumDocsToInsert; i++) {
        docsToInsert.push_back(BSON("_id" << i));
    }

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments(docsToInsert);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQ(kNumDocsToInsert, response.getN());
    });

    expectInsertsReturnStaleVersionErrors({docsToInsert.begin(), docsToInsert.begin() + 60133});
    expectInsertsReturnSuccess({docsToInsert.begin(), docsToInsert.begin() + 60133});
    expectInsertsReturnSuccess({docsToInsert.begin() + 60133, docsToInsert.end()});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, StaleShardVersionReturnedFromBatchWithSingleMultiWrite) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{write_ops::UpdateOpEntry(
            BSON("_id" << 100),
            write_ops::UpdateModification::parseFromClassicUpdate(BSON("Key" << 100)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName1,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none),
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << MINKEY),
                   BSON("x" << 0)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << 0),
                   BSON("x" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(write_ops::WriteError(
            0,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));
        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(2);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(3, response.getNModified());
}

TEST_F(BatchWriteExecTest,
       RetryableErrorReturnedFromMultiWriteWithShard1AllOKShard2AllStaleShardVersion) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{
            write_ops::UpdateOpEntry(
                BSON("id" << 150),
                write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 1))),
            write_ops::UpdateOpEntry(
                BSON("id" << 200),
                write_ops::UpdateModification::parseFromClassicUpdate(BSON("y" << 2)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    // This allows the batch to target each write operation
    // to a specific shard (kShardName2), to perform this test
    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName1,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none),
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << MINKEY),
                   BSON("sk" << 10)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << 10),
                   BSON("sk" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(write_ops::WriteError(
            0,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));
        response.addToErrDetails(write_ops::WriteError(
            1,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));
        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(2);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(3, response.getNModified());
}

TEST_F(BatchWriteExecTest, RetryableErrorReturnedFromMultiWriteWithShard1Firs) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{
            write_ops::UpdateOpEntry(
                BSON("id" << 150),
                write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 1))),
            write_ops::UpdateOpEntry(
                BSON("id" << 200),
                write_ops::UpdateModification::parseFromClassicUpdate(BSON("y" << 2)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    // This allows the batch to target each write operation
    // to a specific shard (kShardName2), to perform this test
    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName1,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none),
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << MINKEY),
                   BSON("sk" << 10)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << 10),
                   BSON("sk" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(write_ops::WriteError(
            1,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));
        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(write_ops::WriteError(
            0,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));
        return response.toBSON();
    });


    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(2, response.getNModified());
}

TEST_F(BatchWriteExecTest, RetryableErrorReturnedFromMultiWriteWithShard1FirstOKShard2FirstOK) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{
            write_ops::UpdateOpEntry(
                BSON("id" << 150),
                write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 1))),
            write_ops::UpdateOpEntry(
                BSON("id" << 200),
                write_ops::UpdateModification::parseFromClassicUpdate(BSON("y" << 2)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    // This allows the batch to target each write operation
    // to a specific shard (kShardName2), to perform this test
    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName1,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none),
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << MINKEY),
                   BSON("sk" << 10)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << 10),
                   BSON("sk" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(write_ops::WriteError(
            1,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));
        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(write_ops::WriteError(
            1,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));
        return response.toBSON();
    });


    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(2, response.getNModified());
}

TEST_F(BatchWriteExecTest, RetryableErrorReturnedFromWriteWithShard1SSVShard2OK) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{write_ops::UpdateOpEntry(
            BSON("_id" << 150),
            write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 1)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    // This allows the batch to target each write operation to perform this test
    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            if (targetAll) {
                return std::vector{
                    ShardEndpoint(kShardName1,
                                  ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                               CollectionIndexes({epoch, timestamp}, boost::none)),
                                  boost::none),
                    ShardEndpoint(kShardName2,
                                  ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                               CollectionIndexes({epoch, timestamp}, boost::none)),
                                  boost::none)};
            } else {
                return std::vector{
                    ShardEndpoint(kShardName2,
                                  ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                               CollectionIndexes({epoch, timestamp}, boost::none)),
                                  boost::none)};
            }
        }

        bool targetAll = true;
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << MINKEY),
                   BSON("sk" << 10)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("sk" << 10),
                   BSON("sk" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);

        return response;
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.setN(0);
        response.addToErrDetails(write_ops::WriteError(
            0,
            Status(StaleConfigInfo(nss,
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {105, 200}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   ShardId(kShardName2)),
                   "Stale error")));

        // This simulates a migration of the last chunk on shard 1 to shard 2, which means that
        // future rounds on the batchExecutor should only target shard 2
        multiShardNSTargeter.targetAll = false;
        return response.toBSON();
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);
        response.setN(1);
        return response.toBSON();
    });

    auto response = future.default_timed_get();
    ASSERT_OK(response.getTopLevelStatus());
    ASSERT_EQ(1, response.getNModified());
    ASSERT_EQ(1, response.getN());
    ASSERT_FALSE(response.isErrDetailsSet());
}

//
// Test retryable errors
//

TEST_F(BatchWriteExecTest, StaleShardOp) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(1, stats.numStaleShardBatches);
    });

    const std::vector<BSONObj> expected{BSON("x" << 1)};

    expectInsertsReturnStaleVersionErrors(expected);
    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, MultiStaleShardOp) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(3, stats.numStaleShardBatches);
    });

    const std::vector<BSONObj> expected{BSON("x" << 1)};

    // Return multiple StaleShardVersion errors, but less than the give-up number
    for (int i = 0; i < 3; i++) {
        expectInsertsReturnStaleVersionErrors(expected);
    }

    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, TooManyStaleShardOp) {
    // Retry op in exec too many times (without refresh) b/c of stale config (the mock
    // singleShardNSTargeter doesn't report progress on refresh). We should report a no progress
    // error for everything in the batch.
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0).getStatus().code(), ErrorCodes::NoProgressMade);
        ASSERT_EQUALS(response.getErrDetailsAt(1).getStatus().code(), ErrorCodes::NoProgressMade);

        ASSERT_EQUALS(stats.numStaleShardBatches, (1 + kMaxRoundsWithoutProgress));
    });

    // Return multiple StaleShardVersion errors
    for (int i = 0; i < (1 + kMaxRoundsWithoutProgress); i++) {
        expectInsertsReturnStaleVersionErrors({BSON("x" << 1), BSON("x" << 2)});
    }

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, StaleDbOp) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(1, stats.numStaleDbBatches);
    });

    const std::vector<BSONObj> expected{BSON("x" << 1)};

    expectInsertsReturnStaleDbVersionErrors(expected);
    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, MultiStaleDbOp) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(3, stats.numStaleDbBatches);
    });

    const std::vector<BSONObj> expected{BSON("x" << 1)};

    // Return multiple StaleDbVersion errors, but less than the give-up number
    for (int i = 0; i < 3; i++) {
        expectInsertsReturnStaleDbVersionErrors(expected);
    }

    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, TooManyStaleDbOp) {
    // Retry op in exec too many times (without refresh) b/c of stale config (the mock
    // singleShardNSTargeter doesn't report progress on refresh). We should report a no progress
    // error for everything in the batch.
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0).getStatus().code(), ErrorCodes::NoProgressMade);
        ASSERT_EQUALS(response.getErrDetailsAt(1).getStatus().code(), ErrorCodes::NoProgressMade);

        ASSERT_EQUALS(stats.numStaleDbBatches, (1 + kMaxRoundsWithoutProgress));
    });

    // Return multiple StaleDbVersion errors
    for (int i = 0; i < (1 + kMaxRoundsWithoutProgress); i++) {
        expectInsertsReturnStaleDbVersionErrors({BSON("x" << 1), BSON("x" << 2)});
    }

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, RetryableWritesLargeBatch) {
    // A retryable error without a txnNumber is not retried.

    const int kNumDocsToInsert = 100'000;
    const std::string kDocValue(200, 'x');

    std::vector<BSONObj> docsToInsert;
    docsToInsert.reserve(kNumDocsToInsert);
    for (int i = 0; i < kNumDocsToInsert; i++) {
        docsToInsert.push_back(BSON("_id" << i << "someLargeKeyToWasteSpace" << kDocValue));
    }

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments(docsToInsert);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQUALS(response.getN(), kNumDocsToInsert);
        ASSERT_EQUALS(stats.numRounds, 2);
    });

    expectInsertsReturnSuccess(docsToInsert.begin(), docsToInsert.begin() + 63791);
    expectInsertsReturnSuccess(docsToInsert.begin() + 63791, docsToInsert.end());

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, RetryableErrorNoTxnNumber) {
    // A retryable error without a txnNumber is not retried.

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    BatchedCommandResponse retryableErrResponse;
    retryableErrResponse.setStatus({ErrorCodes::NotWritablePrimary, "mock retryable error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0).getStatus().code(),
                      retryableErrResponse.toStatus().code());
        ASSERT(response.getErrDetailsAt(0).getStatus().reason().find(
                   retryableErrResponse.toStatus().reason()) != std::string::npos);
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, retryableErrResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, RetryableErrorTxnNumber) {
    // A retryable error with a txnNumber is automatically retried.

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    BatchedCommandResponse retryableErrResponse;
    retryableErrResponse.setStatus({ErrorCodes::NotWritablePrimary, "mock retryable error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT(!response.isErrDetailsSet());
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, retryableErrResponse);
    expectInsertsReturnSuccess({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, NonRetryableErrorTxnNumber) {
    // A non-retryable error with a txnNumber is not retried.

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    BatchedCommandResponse nonRetryableErrResponse;
    nonRetryableErrResponse.setStatus({ErrorCodes::UnknownError, "mock non-retryable error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0).getStatus().code(),
                      nonRetryableErrResponse.toStatus().code());
        ASSERT(response.getErrDetailsAt(0).getStatus().reason().find(
                   nonRetryableErrResponse.toStatus().reason()) != std::string::npos);
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, nonRetryableErrResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, StaleEpochIsNotRetryable) {
    // A StaleEpoch error is not retried.

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(5);

    BatchedCommandResponse nonRetryableErrResponse;
    nonRetryableErrResponse.setStatus({ErrorCodes::StaleEpoch, "mock stale epoch error"});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQ(0, response.getN());
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0).getStatus().code(),
                      nonRetryableErrResponse.toStatus().code());
        ASSERT(response.getErrDetailsAt(0).getStatus().reason().find(
                   nonRetryableErrResponse.toStatus().reason()) != std::string::npos);
        ASSERT_EQ(1, stats.numRounds);
    });

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, nonRetryableErrResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, TenantMigrationAbortedErrorOrderedOp) {
    const std::vector<BSONObj> expected{BSON("x" << 1), BSON("x" << 2), BSON("x" << 3)};
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments(expected);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(1, stats.numTenantMigrationAbortedErrors);
    });

    expectInsertsReturnTenantMigrationAbortedErrors(expected, expected.size());
    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, TenantMigrationAbortedErrorUnorderedOp) {
    const std::vector<BSONObj> expected{BSON("x" << 1), BSON("x" << 2), BSON("x" << 3)};
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments(expected);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(1, stats.numTenantMigrationAbortedErrors);
    });

    expectInsertsReturnTenantMigrationAbortedErrors(expected, expected.size());
    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, MultipleTenantMigrationAbortedErrorUnorderedOp) {
    const std::vector<BSONObj> expected{BSON("x" << 1), BSON("x" << 2), BSON("x" << 3)};
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments(expected);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    const int numTenantMigrationAbortedErrors = 3;

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(numTenantMigrationAbortedErrors, stats.numTenantMigrationAbortedErrors);
    });

    for (int i = 0; i < numTenantMigrationAbortedErrors; i++) {
        expectInsertsReturnTenantMigrationAbortedErrors(expected, expected.size());
    }
    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, MultipleTenantMigrationAbortedErrorOrderedOp) {
    const std::vector<BSONObj> expected{BSON("x" << 1), BSON("x" << 2), BSON("x" << 3)};
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments(expected);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    const int numTenantMigrationAbortedErrors = 3;

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(numTenantMigrationAbortedErrors, stats.numTenantMigrationAbortedErrors);
    });

    for (int i = 0; i < numTenantMigrationAbortedErrors; i++) {
        expectInsertsReturnTenantMigrationAbortedErrors(expected, expected.size());
    }
    expectInsertsReturnSuccess(expected);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, PartialTenantMigrationAbortedErrorOrderedOp) {
    const std::vector<BSONObj> expected{BSON("x" << 1), BSON("x" << 2), BSON("x" << 3)};
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments(expected);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(1, stats.numTenantMigrationAbortedErrors);
    });

    const std::vector<BSONObj> expected_retries{BSON("x" << 2), BSON("x" << 3)};
    int numberOfFailedOps = expected_retries.size();
    expectInsertsReturnTenantMigrationAbortedErrors(expected, numberOfFailedOps);
    expectInsertsReturnSuccess(expected_retries);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTest, PartialTenantMigrationErrorUnorderedOp) {
    const std::vector<BSONObj> expected{BSON("x" << 1), BSON("x" << 2), BSON("x" << 3)};
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments(expected);
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(1, stats.numTenantMigrationAbortedErrors);
    });

    const std::vector<BSONObj> expected_retries{BSON("x" << 2), BSON("x" << 3)};
    int numberOfFailedOps = expected_retries.size();
    expectInsertsReturnTenantMigrationAbortedErrors(expected, numberOfFailedOps);
    expectInsertsReturnSuccess(expected_retries);

    future.default_timed_get();
}

/**
 * Mimics a two shard backend with a targeting error on the first shard.
 */
class BatchWriteExecTargeterErrorTest : public ShardingTestFixture {
public:
    BatchWriteExecTargeterErrorTest() = default;
    ~BatchWriteExecTargeterErrorTest() = default;

    void setUp() override {
        ShardingTestFixture::setUp();
        setRemote(HostAndPort("ClientHost", 12345));

        // Set up the RemoteCommandTargeter for the config shard
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost1), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost1));
            targeter->setFindHostReturnValue(
                {ErrorCodes::ShardNotFound,
                 str::stream() << "Shard " << kShardName1 << " not found"});
            return targeter;
        }());

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost2), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost2));
            targeter->setFindHostReturnValue(kTestShardHost2);
            return targeter;
        }());

        // Set up the shard registry to contain the fake shards
        setupShards({[] {
                         ShardType shardType;
                         shardType.setName(kShardName1);
                         shardType.setHost(kTestShardHost1.toString());
                         return shardType;
                     }(),
                     [] {
                         ShardType shardType;
                         shardType.setName(kShardName2);
                         shardType.setHost(kTestShardHost2.toString());
                         return shardType;
                     }()});
    }

    const NamespaceString nss{"foo.bar"};
};

TEST_F(BatchWriteExecTargeterErrorTest, TargetedFailedAndErrorResponse) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{write_ops::UpdateOpEntry(
            BSON("_id" << 100),
            write_ops::UpdateModification::parseFromClassicUpdate(BSON("Key" << 100)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName1,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none),
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << MINKEY),
                   BSON("x" << 0)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << 0),
                   BSON("x" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);
        ASSERT(response.isErrDetailsSet());
        auto code = response.getErrDetailsAt(0).getStatus().code();
        ASSERT_EQUALS(code, ErrorCodes::MultipleErrorsOccurred);
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.addToErrDetails(
            write_ops::WriteError(0, {ErrorCodes::UnknownError, "mock non-retryable error"}));
        return response.toBSON();
    });

    future.default_timed_get();
}

class BatchWriteExecTransactionTargeterErrorTest : public BatchWriteExecTest {
public:
    const TxnNumber kTxnNumber = 5;
    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    void setUp() override {
        BatchWriteExecTest::setUp();

        setRemote(HostAndPort("ClientHost", 12345));

        // Set up the RemoteCommandTargeter for the config shard
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost1), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setFindHostReturnValue(
                {ErrorCodes::ShardNotFound,
                 str::stream() << "Shard " << kShardName1 << " not found"});
            return targeter;
        }());

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost2), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost2));
            targeter->setFindHostReturnValue(kTestShardHost2);
            return targeter;
        }());

        // Set up the shard registry to contain the fake shards
        setupShards({[] {
                         ShardType shardType;
                         shardType.setName(kShardName1);
                         shardType.setHost(kTestShardHost1.toString());
                         return shardType;
                     }(),
                     [] {
                         ShardType shardType;
                         shardType.setName(kShardName2);
                         shardType.setHost(kTestShardHost2.toString());
                         return shardType;
                     }()});

        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(kTxnNumber);
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        VectorClock::get(getServiceContext())->advanceClusterTime_forTest(kInMemoryLogicalTime);

        _scopedSession.emplace(operationContext());

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());
    }

    void tearDown() override {
        _scopedSession.reset();
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

        BatchWriteExecTest::tearDown();
    }

    const NamespaceString nss{"foo.bar"};

private:
    boost::optional<RouterOperationContextSession> _scopedSession;
};

TEST_F(BatchWriteExecTransactionTargeterErrorTest, TargetedFailedAndErrorResponseInTransaction) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{write_ops::UpdateOpEntry(
            BSON("_id" << 100),
            write_ops::UpdateModification::parseFromClassicUpdate(BSON("Key" << 100)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName1,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none),
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << MINKEY),
                   BSON("x" << 0)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << 0),
                   BSON("x" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);
        ASSERT(response.isErrDetailsSet());
        auto code = response.getErrDetailsAt(0).getStatus().code();
        ASSERT_EQUALS(code, ErrorCodes::ShardNotFound);
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);
        BSONObjBuilder bob;

        bob.append("ok", 0);
        bob.append("code", ErrorCodes::UnknownError);
        bob.append("codeName", ErrorCodes::errorString(ErrorCodes::UnknownError));

        // Because this is the transaction-specific fixture, return transaction metadata in
        // the response.
        TxnResponseMetadata txnResponseMetadata(false /* readOnly */);
        txnResponseMetadata.serialize(&bob);

        return bob.obj();
    });

    future.default_timed_get();

    // It is expected that the transaction will leave the request of Shard2 without processing, so,
    // the baton is manually run because there is no other blocking call on the operation context,
    // making the cleanup as scheduled by the destructor of MultiStatementTransactionRequestsSender.
    operationContext()->getBaton()->run(getServiceContext()->getPreciseClockSource());
}

class BatchWriteExecTransactionMultiShardTest : public BatchWriteExecTest {
public:
    const TxnNumber kTxnNumber = 5;
    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    void setUp() override {
        BatchWriteExecTest::setUp();

        setRemote(HostAndPort("ClientHost", 12345));

        // Set up the RemoteCommandTargeter for the config shard
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost1), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost1));
            targeter->setFindHostReturnValue(kTestShardHost1);
            return targeter;
        }());

        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost2), [] {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost2));
            targeter->setFindHostReturnValue(kTestShardHost2);
            return targeter;
        }());

        // Set up the shard registry to contain the fake shards
        setupShards({[] {
                         ShardType shardType;
                         shardType.setName(kShardName1);
                         shardType.setHost(kTestShardHost1.toString());
                         return shardType;
                     }(),
                     [] {
                         ShardType shardType;
                         shardType.setName(kShardName2);
                         shardType.setHost(kTestShardHost2.toString());
                         return shardType;
                     }()});

        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(kTxnNumber);
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        VectorClock::get(getServiceContext())->advanceClusterTime_forTest(kInMemoryLogicalTime);

        _scopedSession.emplace(operationContext());

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());
    }

    void tearDown() override {
        _scopedSession.reset();
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

        BatchWriteExecTest::tearDown();
    }

private:
    boost::optional<RouterOperationContextSession> _scopedSession;
};

TEST_F(BatchWriteExecTransactionMultiShardTest, TargetedSucceededAndErrorResponseInTransaction) {
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{write_ops::UpdateOpEntry(
            BSON("_id" << 100),
            write_ops::UpdateModification::parseFromClassicUpdate(BSON("Key" << 100)))});
        return updateOp;
    }());
    request.setWriteConcern(BSONObj());

    const static auto epoch = OID::gen();
    const static Timestamp timestamp(2);

    class MultiShardTargeter : public MockNSTargeter {
    public:
        using MockNSTargeter::MockNSTargeter;

        std::vector<ShardEndpoint> targetUpdate(OperationContext* opCtx,
                                                const BatchItemRef& itemRef) const override {
            return std::vector{
                ShardEndpoint(kShardName1,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none),
                ShardEndpoint(kShardName2,
                              ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                           CollectionIndexes({epoch, timestamp}, boost::none)),
                              boost::none)};
        }
    };

    MultiShardTargeter multiShardNSTargeter(
        nss,
        {MockRange(ShardEndpoint(kShardName1,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {100, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << MINKEY),
                   BSON("x" << 0)),
         MockRange(ShardEndpoint(kShardName2,
                                 ShardVersion(ChunkVersion({epoch, timestamp}, {101, 200}),
                                              CollectionIndexes({epoch, timestamp}, boost::none)),
                                 boost::none),
                   BSON("x" << 0),
                   BSON("x" << MAXKEY))});

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), multiShardNSTargeter, request, &response, &stats);
        ASSERT(response.isErrDetailsSet());
        auto code = response.getErrDetailsAt(0).getStatus().code();
        ASSERT_EQUALS(code, ErrorCodes::UnknownError);
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost1, request.target);
        BSONObjBuilder bob;

        bob.append("ok", 0);
        bob.append("code", ErrorCodes::UnknownError);
        bob.append("codeName", ErrorCodes::errorString(ErrorCodes::UnknownError));

        // Because this is the transaction-specific fixture, return transaction metadata in
        // the response.
        TxnResponseMetadata txnResponseMetadata(false /* readOnly */);
        txnResponseMetadata.serialize(&bob);

        return bob.obj();
    });
    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(kTestShardHost2, request.target);
        BSONObjBuilder bob;

        bob.append("ok", 0);
        bob.append("code", ErrorCodes::UnknownError);
        bob.append("codeName", ErrorCodes::errorString(ErrorCodes::UnknownError));

        // Because this is the transaction-specific fixture, return transaction metadata in
        // the response.
        TxnResponseMetadata txnResponseMetadata(false /* readOnly */);
        txnResponseMetadata.serialize(&bob);

        return bob.obj();
    });

    future.default_timed_get();

    // It is expected that the transaction will leave the request of Shard2 without processing, so,
    // the baton is manually run because there is no other blocking call on the operation context,
    // making the cleanup as scheduled by the destructor of MultiStatementTransactionRequestsSender.
    operationContext()->getBaton()->run(getServiceContext()->getPreciseClockSource());
}

/**
 * General transaction tests.
 */
class BatchWriteExecTransactionTest : public BatchWriteExecTest {
public:
    const TxnNumber kTxnNumber = 5;
    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    void setUp() override {
        BatchWriteExecTest::setUp();

        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(kTxnNumber);
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        VectorClock::get(getServiceContext())->advanceClusterTime_forTest(kInMemoryLogicalTime);

        _scopedSession.emplace(operationContext());

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());
    }

    void tearDown() override {
        _scopedSession.reset();
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

        BatchWriteExecTest::tearDown();
    }

    void expectInsertsReturnStaleVersionErrors(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            BSONObjBuilder bob;

            bob.appendElementsUnique(
                expectInsertsReturnStaleVersionErrorsBase(nss, expected, request));

            // Because this is the transaction-specific fixture, return transaction metadata in
            // the response.
            TxnResponseMetadata txnResponseMetadata(false /* readOnly */);
            txnResponseMetadata.serialize(&bob);

            return bob.obj();
        });
    }

    void expectInsertsReturnTransientTxnErrors(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS(nss.db(), request.dbname);

            const auto opMsgRequest(OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));
            const auto actualBatchedInsert(BatchedCommandRequest::parseInsert(opMsgRequest));
            ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().ns());

            const auto& inserted = actualBatchedInsert.getInsertRequest().getDocuments();
            ASSERT_EQUALS(expected.size(), inserted.size());

            auto itInserted = inserted.begin();
            auto itExpected = expected.begin();

            for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
            }

            BSONObjBuilder bob;

            bob.append("ok", 0);
            bob.append("errorLabels", BSON_ARRAY("TransientTransactionError"));
            bob.append("code", ErrorCodes::WriteConflict);
            bob.append("codeName", ErrorCodes::errorString(ErrorCodes::WriteConflict));

            // Because this is the transaction-specific fixture, return transaction metadata in
            // the response.
            TxnResponseMetadata txnResponseMetadata(false /* readOnly */);
            txnResponseMetadata.serialize(&bob);

            return bob.obj();
        });
    }

private:
    boost::optional<RouterOperationContextSession> _scopedSession;
};

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchThrows_CommandError) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::UnknownError, response.getErrDetailsAt(0).getStatus().code());
    });

    BatchedCommandResponse failedResponse;
    failedResponse.setStatus({ErrorCodes::UnknownError, "dummy error"});

    expectInsertsReturnError({BSON("x" << 1), BSON("x" << 2)}, failedResponse);

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_WriteError) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::StaleConfig, response.getErrDetailsAt(0).getStatus().code());
    });

    // Any write error works, using SSV for convenience.
    expectInsertsReturnStaleVersionErrors({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_WriteErrorOrdered) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::StaleConfig, response.getErrDetailsAt(0).getStatus().code());
    });

    // Any write error works, using SSV for convenience.
    expectInsertsReturnStaleVersionErrors({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_TransientTxnError) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        ASSERT_THROWS(BatchWriteExec::executeBatch(
                          operationContext(), singleShardNSTargeter, request, &response, &stats),
                      WriteConflictException);
    });

    expectInsertsReturnTransientTxnErrors({BSON("x" << 1), BSON("x" << 2)});

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_DispatchError) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        BatchWriteExec::executeBatch(
            operationContext(), singleShardNSTargeter, request, &response, &stats);

        ASSERT(response.isErrDetailsSet());
        ASSERT_GT(response.sizeErrDetails(), 0u);
        ASSERT_EQ(ErrorCodes::CallbackCanceled, response.getErrDetailsAt(0).getStatus().code());
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::CallbackCanceled, "simulating executor cancel for test");
    });

    future.default_timed_get();
}

TEST_F(BatchWriteExecTransactionTest, ErrorInBatchSets_TransientDispatchError) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(false);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSONObj());

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;

        ASSERT_THROWS_CODE(
            BatchWriteExec::executeBatch(
                operationContext(), singleShardNSTargeter, request, &response, &stats),
            AssertionException,
            ErrorCodes::InterruptedAtShutdown);
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::InterruptedAtShutdown, "simulating shutdown for test");
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
