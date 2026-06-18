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
#include "mongo/db/sharding_environment/shard_handle.h"
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

    /**
     * Converts a list of ShardHandles into a list of ShardRefs using the test's OperationContext,
     * so it can be passed to the transaction coordinator APIs.
     */
    std::vector<ShardRef> transformToShardRefs(const std::vector<ShardHandle>& handles) const {
        std::vector<ShardRef> shardRefs;
        shardRefs.reserve(handles.size());
        for (const auto& handle : handles) {
            shardRefs.push_back(handle.toShardRef(operationContext()));
        }
        return shardRefs;
    }

    /**
     * Converts a set of ShardHandles into a set of ShardRefs using the test's OperationContext, so
     * it can be passed to the transaction coordinator APIs.
     */
    std::set<ShardRef> transformToShardRefs(const std::set<ShardHandle>& handles) const {
        std::set<ShardRef> shardRefs;
        for (const auto& handle : handles) {
            shardRefs.insert(handle.toShardRef(operationContext()));
        }
        return shardRefs;
    }

    // TODO SERVER-128815: use non empty optionals for UUIDs once feature is ready

    const std::vector<ShardHandle> kOneShardHandleList{ShardHandle{ShardId{"s1"}, boost::none}};
    const std::set<ShardHandle> kOneShardHandleSet{ShardHandle{ShardId{"s1"}, boost::none}};
    const std::vector<ShardHandle> kTwoShardHandleList{ShardHandle{ShardId{"s1"}, boost::none},
                                                       ShardHandle{ShardId{"s2"}, boost::none}};
    const std::set<ShardHandle> kTwoShardHandleSet{ShardHandle{ShardId{"s1"}, boost::none},
                                                   ShardHandle{ShardId{"s2"}, boost::none}};
    const std::vector<ShardHandle> kThreeShardHandleList{ShardHandle{ShardId{"s1"}, boost::none},
                                                         ShardHandle{ShardId{"s2"}, boost::none},
                                                         ShardHandle{ShardId{"s3"}, boost::none}};
    const std::set<ShardHandle> kThreeShardHandleSet{ShardHandle{ShardId{"s1"}, boost::none},
                                                     ShardHandle{ShardId{"s2"}, boost::none},
                                                     ShardHandle{ShardId{"s3"}, boost::none}};

    const Status kRetryableError{ErrorCodes::HostUnreachable,
                                 "Retryable error for coordinator test"};
};

}  // namespace mongo
