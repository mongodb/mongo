// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/modules.h"


#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Implements common functionality shared across the various transaction coordinator unit-tests.
 */
class TransactionCoordinatorTestFixture : public ShardServerTestFixture {
protected:
    explicit TransactionCoordinatorTestFixture(Options options = {})
        : ShardServerTestFixture(std::move(options)) {}

    void setUp() override;
    void tearDown() override;

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override;

    void assertCommandSentAndRespondWith(std::string_view commandName,
                                         const StatusWith<BSONObj>& response,
                                         boost::optional<WriteConcernOptions> expectedWriteConcern);
    /**
     * These tests use the network task executor mock, which doesn't automatically execute tasks,
     * which are scheduled with delay. This helper function advances the clock by 1 second (which is
     * the maximum back-off in the transaction coordinator) and causes any retries to run.
     */
    void advanceClockAndExecuteScheduledTasks();


    /**
     * Associates metatadata with the provided client. Metadata fields have appName prepended to
     * thier value.
     */
    static void associateClientMetadata(Client* client, std::string appName);

    const std::vector<ShardId> kOneShardIdList{{"s1"}};
    const std::set<ShardId> kOneShardIdSet{{"s1"}};
    const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};
    const std::set<ShardId> kTwoShardIdSet{{"s1"}, {"s2"}};
    const std::vector<ShardId> kThreeShardIdList{{"s1"}, {"s2"}, {"s3"}};
    const std::set<ShardId> kThreeShardIdSet{{"s1"}, {"s2"}, {"s3"}};

    const Status kRetryableError{ErrorCodes::HostUnreachable,
                                 "Retryable error for coordinator test"};
};

}  // namespace mongo
