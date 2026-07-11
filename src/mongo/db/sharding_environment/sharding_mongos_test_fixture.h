// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_mock.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <tuple>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Sets up the mocked out objects for testing the replica-set backed catalog manager and catalog
 * client.
 */
class ShardingTestFixture : public ShardingTestFixtureCommon {
protected:
    ShardingTestFixture();
    explicit ShardingTestFixture(bool withMockCatalogCache)
        : ShardingTestFixture{withMockCatalogCache, nullptr} {}
    ShardingTestFixture(bool withMockCatalogCache,
                        std::unique_ptr<ScopedGlobalServiceContextForTest> scopedGlobalContext);

    ~ShardingTestFixture() override;

    /**
     * Returns the mock targeter for the config server. Useful to use like so,
     *
     *     configTargeterMock()->setFindHostReturnValue(HostAndPort);
     *     configTargeterMock()->setFindHostReturnValue({ErrorCodes::InternalError, "can't target"})
     *
     * Remote calls always need to resolve a host with RemoteCommandTargeterMock::findHost, so it
     * must be set.
     */
    std::shared_ptr<RemoteCommandTargeterMock> configTargeter();

    // Syntactic sugar for getting sharding components off the Grid, if they have been initialized.

    ShardingCatalogClient* catalogClient() const;
    std::shared_ptr<executor::TaskExecutor> executor() const;

    /**
     * Same as the onCommand* variants, but expects the request to be placed on the arbitrary
     * executor of the Grid's executorPool.
     */
    void onCommandForPoolExecutor(executor::NetworkTestEnv::OnCommandFunction func);

    /**
     * Setup the shard registry to contain the given shards until the next reload.
     */
    void setupShards(const std::vector<ShardType>& shards);

    /**
     * Wait for the shards listing command to be run and returns the specified set of shards.
     */
    void expectGetShards(const std::vector<ShardType>& shards);

    /**
     * Wait for a single insert request and ensures that the items being inserted exactly match the
     * expected items. Responds with a success status.
     */
    void expectInserts(const NamespaceString& nss, const std::vector<BSONObj>& expected);

    /**
     * Waits for a count command and returns a response reporting the given number of documents
     * as the result of the count, or an error.
     */
    void expectCount(const HostAndPort& configHost,
                     const NamespaceString& expectedNs,
                     const BSONObj& expectedQuery,
                     const StatusWith<long long>& response);

    /**
     * Expects a find command on configHost's 'config' database and returns an array of objects
     * 'obj' as a response.
     */
    void expectFindSendBSONObjVector(const HostAndPort& configHost, std::vector<BSONObj> obj);

    /**
     * Expects an update call, which changes the specified collection's namespace contents to match
     * those of the input argument.
     */
    void expectUpdateCollection(const HostAndPort& expectedHost,
                                const CollectionType& coll,
                                bool expectUpsert = true);

    void shutdownExecutor();

    /**
     * Checks that the given command has the expected settings for read after opTime.
     */
    void checkReadConcern(const BSONObj& cmdObj,
                          const Timestamp& expectedTS,
                          long long expectedTerm) const;

    /**
     * Mocks an error cursor response from a remote with the given 'status'.
     */
    BSONObj createErrorCursorResponse(Status status) {
        invariant(!status.isOK());
        BSONObjBuilder result;
        status.serializeErrorToBSON(&result);
        result.appendBool("ok", false);
        return result.obj();
    }

    /**
     * Mocks an error response from a remote that includes the error labels 'SystemOverloadError'
     * and 'RetryableError'
     */
    static BSONObj createErrorSystemOverloaded(
        ErrorCodes::Error errorCode, boost::optional<Milliseconds> baseBackoffMS = boost::none) {
        BSONObjBuilder bob;
        bob.append("ok", 0.0);
        bob.append("code", errorCode);
        bob.append("errmsg", "overloaded");
        bob.append("codeName", ErrorCodes::errorString(errorCode));
        {
            BSONArrayBuilder arrayBuilder = bob.subarrayStart(kErrorLabelsFieldName);
            arrayBuilder.append(ErrorLabel::kSystemOverloadedError);
            arrayBuilder.append(ErrorLabel::kRetryableError);
        }
        if (baseBackoffMS) {
            bob.append("baseBackoffMS", static_cast<long long>(baseBackoffMS->count()));
        }
        return bob.obj();
    }


private:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override;

    // For the Grid's fixed executor.
    std::shared_ptr<executor::TaskExecutor> _fixedExecutor;

    // For the Grid's arbitrary executor in its executorPool.
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnvForPool;
};

class ShardingTestFixtureWithMockCatalogCache : public ShardingTestFixture {
public:
    ShardingTestFixtureWithMockCatalogCache()
        : ShardingTestFixture(true /*withMockCatalogCache*/) {}

    CatalogCacheMock* getCatalogCacheMock() {
        return checked_cast<CatalogCacheMock*>(Grid::get(operationContext())->catalogCache());
    }
};

}  // namespace mongo
