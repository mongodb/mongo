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

#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/mongod_and_mongos_server_parameters_gen.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class SyncShardVersionRetry : public ShardingTestFixture {
public:
    NamespaceString nss() const {
        return NamespaceString::createNamespaceString_forTest("test", "foo");
    }

    StringData desc() const {
        return "shardVersionRetryTest"_sd;
    }

    ServiceContext* service() {
        return operationContext()->getServiceContext();
    }

    std::shared_ptr<executor::TaskExecutor> getExecutor() const {
        return executor();
    }

private:
    CancellationSource _cancellationSource;
};

TEST_F(SyncShardVersionRetry, NoErrorsWithVoidReturnTypeCallback) {
    CancellationSource cancellationSource;
    auto token = cancellationSource.token();
    auto catalogCache = Grid::get(service())->catalogCache();

    shardVersionRetry(operationContext(), catalogCache, nss(), desc(), [&]() {});
}

TEST_F(SyncShardVersionRetry, NoErrorsWithNonVoidReturnTypeCallback) {
    CancellationSource cancellationSource;
    auto token = cancellationSource.token();
    auto catalogCache = Grid::get(service())->catalogCache();

    auto retVal = shardVersionRetry(
        operationContext(), catalogCache, nss(), desc(), [&]() { return "pass"; });

    ASSERT_EQ("pass", retVal);
}

TEST_F(SyncShardVersionRetry, LimitedStaleErrorsShouldReturnCorrectValue) {
    CancellationSource cancellationSource;
    auto token = cancellationSource.token();
    auto catalogCache = Grid::get(service())->catalogCache();

    size_t tries = 0;
    auto retVal = shardVersionRetry(operationContext(), catalogCache, nss(), desc(), [&]() {
        if (++tries < 5) {
            const CollectionGeneration gen1(OID::gen(), Timestamp(1, 0));
            const CollectionGeneration gen2(OID::gen(), Timestamp(1, 0));
            uassert(StaleConfigInfo(
                        nss(),
                        ShardVersionFactory::make(ChunkVersion(gen1, {5, 23}),
                                                  boost::optional<CollectionIndexes>(boost::none)),
                        ShardVersionFactory::make(ChunkVersion(gen2, {6, 99}),
                                                  boost::optional<CollectionIndexes>(boost::none)),
                        ShardId("sB")),
                    "testX",
                    false);
        }

        return 10;
    });

    ASSERT_EQ(10, retVal);

    size_t altMaxNumRetries = 6;
    tries = 0;
    retVal = shardVersionRetry(
        operationContext(),
        catalogCache,
        nss(),
        desc(),
        [&]() {
            if (++tries < altMaxNumRetries / 2) {
                const CollectionGeneration gen1(OID::gen(), Timestamp(1, 0));
                const CollectionGeneration gen2(OID::gen(), Timestamp(1, 0));
                uassert(
                    StaleConfigInfo(
                        nss(),
                        ShardVersionFactory::make(ChunkVersion(gen1, {5, 23}),
                                                  boost::optional<CollectionIndexes>(boost::none)),
                        ShardVersionFactory::make(ChunkVersion(gen2, {6, 99}),
                                                  boost::optional<CollectionIndexes>(boost::none)),
                        ShardId("sB")),
                    "testX",
                    false);
            }

            return 10;
        },
        altMaxNumRetries);

    ASSERT_EQ(10, retVal);
}

TEST_F(SyncShardVersionRetry, ExhaustedRetriesShouldThrowOriginalException) {
    CancellationSource cancellationSource;
    auto token = cancellationSource.token();
    auto catalogCache = Grid::get(service())->catalogCache();

    int tries = 0;
    auto shardVersionRetryInvocation = [&]() {
        return shardVersionRetry(operationContext(), catalogCache, nss(), desc(), [&]() {
            if (++tries < 2 * gMaxNumStaleVersionRetries.load()) {
                uassert(StaleDbRoutingVersion(nss().dbName(),
                                              DatabaseVersion(UUID::gen(), Timestamp(2, 3)),
                                              DatabaseVersion(UUID::gen(), Timestamp(5, 3))),
                        "testX",
                        false);
            }
            return 10;
        });
    };

    ASSERT_THROWS_CODE(shardVersionRetryInvocation(), DBException, ErrorCodes::StaleDbVersion);
}

TEST_F(SyncShardVersionRetry, AltExhaustedRetriesShouldThrowOriginalException) {
    CancellationSource cancellationSource;
    auto token = cancellationSource.token();
    auto catalogCache = Grid::get(service())->catalogCache();

    size_t altMaxNumRetries = 2;

    size_t tries = 0;
    auto shardVersionRetryInvocation = [&]() {
        return shardVersionRetry(
            operationContext(),
            catalogCache,
            nss(),
            desc(),
            [&]() {
                if (++tries < 2 * altMaxNumRetries) {
                    uassert(StaleDbRoutingVersion(nss().dbName(),
                                                  DatabaseVersion(UUID::gen(), Timestamp(2, 3)),
                                                  DatabaseVersion(UUID::gen(), Timestamp(5, 3))),
                            "testX",
                            false);
                }
                return 10;
            },
            altMaxNumRetries);
    };

    ASSERT_THROWS_CODE(shardVersionRetryInvocation(), DBException, ErrorCodes::StaleDbVersion);
}

TEST_F(SyncShardVersionRetry, ShouldNotBreakOnTimeseriesBucketNamespaceRewrite) {
    CancellationSource cancellationSource;
    auto token = cancellationSource.token();
    auto catalogCache = Grid::get(service())->catalogCache();

    size_t tries = 0;
    auto retVal = shardVersionRetry(operationContext(), catalogCache, nss(), desc(), [&]() {
        if (++tries < 5) {
            const CollectionGeneration gen1(OID::gen(), Timestamp(1, 0));
            const CollectionGeneration gen2(OID::gen(), Timestamp(1, 0));
            uassert(StaleConfigInfo(
                        nss().makeTimeseriesBucketsNamespace(),
                        ShardVersionFactory::make(ChunkVersion(gen1, {5, 23}),
                                                  boost::optional<CollectionIndexes>(boost::none)),
                        ShardVersionFactory::make(ChunkVersion(gen2, {6, 99}),
                                                  boost::optional<CollectionIndexes>(boost::none)),
                        ShardId("sB")),
                    "testX",
                    false);
        }

        return 10;
    });

    ASSERT_EQ(10, retVal);
}

}  // namespace
}  // namespace mongo
