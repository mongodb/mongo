/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

// IWYU pragma: no_include "cxxabi.h"
#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
#include <future>
#include <system_error>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/migration_batch_fetcher.h"
#include "mongo/db/s/migration_batch_mock_inserter.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using unittest::assertGet;
using namespace std::chrono_literals;

const ConnectionString kDonorConnStr =
    ConnectionString::forReplicaSet("Donor",
                                    {HostAndPort("DonorHost1:1234"),
                                     HostAndPort{"DonorHost2:1234"},
                                     HostAndPort{"DonorHost3:1234"}});
const ConnectionString kRecipientConnStr =
    ConnectionString::forReplicaSet("Recipient",
                                    {HostAndPort("RecipientHost1:1234"),
                                     HostAndPort("RecipientHost2:1234"),
                                     HostAndPort("RecipientHost3:1234")});

class MigrationBatchFetcherTestFixture : public ShardServerTestFixture {

protected:
    /**
     * Sets up the task executor as well as a TopologyListenerMock for each unit test.
     */
    void setUp() override {
        ShardServerTestFixture::setUp();

        {
            auto donorShard = assertGet(
                shardRegistry()->getShard(operationContext(), kDonorConnStr.getSetName()));
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setConnectionStringReturnValue(kDonorConnStr);
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setFindHostReturnValue(kDonorConnStr.getServers()[0]);
        }
    }

    void tearDown() override {
        ShardServerTestFixture::tearDown();
    }

    /**
     * Instantiates a BSON object in which both "_id" and "X" are set to value.
     */
    static BSONObj createDocument(int value) {
        return BSON("_id" << value << "X" << value);
    }
    static BSONObj createEmpty() {
        return BSONObj{};
    }
    /**
     * Creates a list of documents to clone.
     */
    static std::vector<BSONObj> createDocumentsToClone() {
        return {createDocument(1), createDocument(2), createDocument(3)};
    }

    /**
     * Creates a list of documents to clone and converts it to a BSONArray.
     */
    static BSONArray createDocumentsToCloneArray() {
        BSONArrayBuilder arrayBuilder;
        for (auto& doc : createDocumentsToClone()) {
            arrayBuilder.append(doc);
        }
        return arrayBuilder.arr();
    }
    static BSONArray createEmptyCloneArray() {
        return BSONArrayBuilder().arr();
    }

    static BSONObj getTerminalBsonObj() {
        return BSON("Status"
                    << "OK"
                    << "ok" << 1 << "objects" << createEmptyCloneArray());
    }

    static BSONObj getBatchBsonObj() {
        return BSON("Status"
                    << "OK"
                    << "ok" << 1 << "objects" << createDocumentsToCloneArray());
    }

private:
    OperationContext* _opCtx;
    ServiceContext* _svcCtx;
    executor::NetworkInterfaceMock* _net;

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() = default;

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {

                ShardType donorShard;
                donorShard.setName(kDonorConnStr.getSetName());
                donorShard.setHost(kDonorConnStr.toString());

                ShardType recipientShard;
                recipientShard.setName(kRecipientConnStr.getSetName());
                recipientShard.setHost(kRecipientConnStr.toString());

                return repl::OpTimeWith<std::vector<ShardType>>({donorShard, recipientShard});
            }
        };

        return std::make_unique<StaticCatalogClient>();
    }
};

auto getOnMigrateCloneCommandCb(BSONObj ret) {
    return [ret](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        ASSERT_EQ(request.cmdObj.getField("_migrateClone").String(), "test.foo");
        return ret;
    };
}

TEST_F(MigrationBatchFetcherTestFixture, BasicEmptyFetchingTest) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    ShardId fromShard{"Donor"};
    auto msid = MigrationSessionId::generate(fromShard, "Recipient");
    auto outerOpCtx = operationContext();
    auto newClient = outerOpCtx->getServiceContext()->makeClient("MigrationCoordinator");

    int concurrency = 30;
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagConcurrencyInChunkMigration", true);
    RAIIServerParameterControllerForTest setMigrationConcurrencyParam{"chunkMigrationConcurrency",
                                                                      concurrency};

    AlternativeClientRegion acr(newClient);
    auto executor =
        Grid::get(outerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
    auto newOpCtxPtr = CancelableOperationContext(
        cc().makeOperationContext(), outerOpCtx->getCancellationToken(), executor);
    auto opCtx = newOpCtxPtr.get();

    auto fetcher = std::make_unique<MigrationBatchFetcher<MigrationBatchMockInserter>>(
        outerOpCtx,
        opCtx,
        nss,
        msid,
        WriteConcernOptions::parse(WriteConcernOptions::Majority).getValue(),
        fromShard,
        ChunkRange{BSON("x" << 1), BSON("x" << 2)},
        UUID::gen(),
        UUID::gen(),
        nullptr,
        true,
        0 /* maxBytesPerThread */);

    // Start asynchronous task for responding to _migrateClone requests.
    // Must name the return of value std::async.  The destructor of std::future joins the
    // asynchrounous task. (If it were left unnamed, the destructor would run inline, and the test
    // would hang forever.)
    auto fut = stdx::async(stdx::launch::async, [&]() {
        // One terminal response for each thread
        for (int i = 0; i < concurrency; ++i) {
            onCommand(getOnMigrateCloneCommandCb(getTerminalBsonObj()));
        }
    });
    fetcher->fetchAndScheduleInsertion();
}

TEST_F(MigrationBatchFetcherTestFixture, BasicFetching) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "foo");
    ShardId fromShard{"Donor"};
    auto msid = MigrationSessionId::generate(fromShard, "Recipient");

    auto outerOpCtx = operationContext();
    auto newClient = outerOpCtx->getServiceContext()->makeClient("MigrationCoordinator");
    AlternativeClientRegion acr(newClient);

    auto executor =
        Grid::get(outerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
    auto newOpCtxPtr = CancelableOperationContext(
        cc().makeOperationContext(), outerOpCtx->getCancellationToken(), executor);
    auto opCtx = newOpCtxPtr.get();


    int concurrency = 30;
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagConcurrencyInChunkMigration", true);
    RAIIServerParameterControllerForTest setMigrationConcurrencyParam{"chunkMigrationConcurrency",
                                                                      concurrency};

    auto fetcher = std::make_unique<MigrationBatchFetcher<MigrationBatchMockInserter>>(
        outerOpCtx,
        opCtx,
        nss,
        msid,
        WriteConcernOptions::parse(WriteConcernOptions::Majority).getValue(),
        fromShard,
        ChunkRange{BSON("x" << 1), BSON("x" << 2)},
        UUID::gen(),
        UUID::gen(),
        nullptr,
        true,
        0 /* maxBytesPerThread */);

    auto fut = stdx::async(stdx::launch::async, [&]() {
        for (int i = 0; i < 8; ++i) {
            onCommand(getOnMigrateCloneCommandCb(getBatchBsonObj()));
        }
        // One terminal response for each thread
        for (int i = 0; i < concurrency; ++i) {
            onCommand(getOnMigrateCloneCommandCb(getTerminalBsonObj()));
        }
    });
    fetcher->fetchAndScheduleInsertion();
}

}  // namespace
}  // namespace mongo
