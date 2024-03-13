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

#pragma once

#include <boost/optional/optional.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/catalog/sharding_catalog_client.h"

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

    void assertCommandSentAndRespondWith(StringData commandName,
                                         const StatusWith<BSONObj>& response,
                                         boost::optional<BSONObj> expectedWriteConcern);
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
