// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_impl.h"

#include "mongo/db/exec/single_doc_lookup/sharded_cluster_local_lookup_eligibility.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/transport/mock_session.h"

namespace mongo::exec::agg {
namespace {

// Verifies LocalLookupEligibilityFactoryImpl's selection on a shard where ShardingState is
// enabled: AlwaysLocalEligibility whenever the connecting client is not internal (i.e. connected
// directly to this shard rather than via mongos/another cluster member); only a routed
// (internal-client) connection consults the catalog cache.
//
// A real (mock) session is attached to each test's own client, rather than using the fixture's
// default session-less client: 'isInternalThreadOrClient' treats a session-less client as
// internal unconditionally, which would mask the isInternalClient() distinction under test here.
class LocalLookupEligibilityFactoryImplShardedTest : public ShardServerTestFixture {
protected:
    ServiceContext::UniqueClient makeSessionedClient() {
        return getServiceContext()->getService()->makeClient(
            "LocalLookupEligibilityFactoryImplShardedTest",
            transport::MockSession::create(nullptr));
    }

    LocalLookupEligibilityFactoryImpl _factory;
};

TEST_F(LocalLookupEligibilityFactoryImplShardedTest, ShardedButNotInternalClientIsAlwaysLocal) {
    // ShardServerTestFixture::setUp() already enables ShardingState. A client that did not pass
    // the internal-client handshake (e.g. it connected directly to the shard, bypassing mongos)
    // must still get AlwaysLocalEligibility: there is no safe way to reason about cross-shard
    // placement for it.
    auto client = makeSessionedClient();
    auto opCtx = client->makeOperationContext();
    opCtx->getClient()->setIsInternalClient(false);

    auto eligibility = _factory.makeLocalLookupEligibility(opCtx.get());
    ASSERT_TRUE(dynamic_cast<AlwaysLocalEligibility*>(eligibility.get()));
}

TEST_F(LocalLookupEligibilityFactoryImplShardedTest,
       ShardedAndInternalClientRoutesThroughCatalogCache) {
    Client::setCheckAuthForInternalClient([](Client*) { return true; });
    auto client = makeSessionedClient();
    auto opCtx = client->makeOperationContext();
    opCtx->getClient()->setIsInternalClient(true);

    auto eligibility = _factory.makeLocalLookupEligibility(opCtx.get());
    ASSERT_TRUE(dynamic_cast<ShardedClusterLocalLookupEligibility*>(eligibility.get()));
}

}  // namespace
}  // namespace mongo::exec::agg
