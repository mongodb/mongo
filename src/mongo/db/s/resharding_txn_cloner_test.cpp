/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding_txn_cloner.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ReshardingTxnClonerTest : public ShardServerTestFixture {
    void setUp() {
        ShardServerTestFixture::setUp();
        for (const auto& shardId : kTwoShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());
    }

    void tearDown() {
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds)
                : ShardingCatalogClientMock(nullptr), _shardIds(std::move(shardIds)) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : _shardIds) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }

        private:
            const std::vector<ShardId> _shardIds;
        };

        return std::make_unique<StaticCatalogClient>(kTwoShardIdList);
    }

protected:
    const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};

    BSONObj makeTxn() {
        return SessionTxnRecord(
                   makeLogicalSessionIdForTest(), 0, repl::OpTime(Timestamp::min(), 0), Date_t())
            .toBSON();
        ;
    }

private:
    static HostAndPort makeHostAndPort(const ShardId& shardId) {
        return HostAndPort(str::stream() << shardId << ":123");
    }
};

TEST_F(ReshardingTxnClonerTest, TxnAggregation) {
    std::vector<BSONObj> expectedTransactions{
        makeTxn(), makeTxn(), makeTxn(), makeTxn(), makeTxn(), makeTxn(), makeTxn()};
    std::vector<BSONObj> retrievedTransactions;

    auto future = launchAsync([&, this] {
        auto fetcher =
            cloneConfigTxnsForResharding(operationContext(),
                                         kTwoShardIdList[1],
                                         Timestamp::max(),
                                         boost::none,
                                         [&](StatusWith<BSONObj> statusWithTransaction) {
                                             auto transaction =
                                                 unittest::assertGet(statusWithTransaction);
                                             retrievedTransactions.push_back(transaction);
                                         });

        fetcher->join();
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                              CursorId{123},
                              std::vector<BSONObj>(expectedTransactions.begin(),
                                                   expectedTransactions.begin() + 4))
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                              CursorId{0},
                              std::vector<BSONObj>(expectedTransactions.begin() + 4,
                                                   expectedTransactions.end()))
            .toBSON(CursorResponse::ResponseType::SubsequentResponse);
    });

    future.default_timed_get();

    ASSERT(std::equal(expectedTransactions.begin(),
                      expectedTransactions.end(),
                      retrievedTransactions.begin(),
                      [](BSONObj a, BSONObj b) { return a.binaryEqual(b); }));
}

TEST_F(ReshardingTxnClonerTest, CursorNotFoundError) {
    std::vector<BSONObj> expectedTransactions{
        makeTxn(), makeTxn(), makeTxn(), makeTxn(), makeTxn(), makeTxn(), makeTxn()};
    std::vector<BSONObj> retrievedTransactions;
    int errorsReturned = 0;
    Status error = Status::OK();

    auto future = launchAsync([&, this] {
        auto fetcher = cloneConfigTxnsForResharding(
            operationContext(),
            kTwoShardIdList[1],
            Timestamp::max(),
            boost::none,
            [&](StatusWith<BSONObj> statusWithTransaction) {
                if (statusWithTransaction.isOK()) {
                    retrievedTransactions.push_back(statusWithTransaction.getValue());
                } else {
                    errorsReturned++;
                    error = statusWithTransaction.getStatus();
                }
            });
        fetcher->join();
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                              CursorId{124},
                              expectedTransactions)
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::CursorNotFound, "Simulate cursor not found error");
    });

    future.default_timed_get();

    ASSERT(std::equal(expectedTransactions.begin(),
                      expectedTransactions.end(),
                      retrievedTransactions.begin(),
                      [](BSONObj a, BSONObj b) { return a.binaryEqual(b); }));
    ASSERT_EQ(errorsReturned, 1);
    ASSERT_EQ(error, ErrorCodes::CursorNotFound);
}

}  // namespace
}  // namespace mongo
