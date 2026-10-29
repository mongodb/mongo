// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_table_cache_gossip_metadata_hook.h"

#include "mongo/db/router_role/gossiped_routing_cache_gen.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/idl/generic_argument_gen.h"

namespace mongo {
namespace {

class RoutingTableCacheGossipMetadataHookTest : public ShardingTestFixtureWithMockCatalogCache {
public:
    void setUp() override {
        ShardingTestFixtureWithMockCatalogCache::setUp();

        gossipHook = std::make_unique<rpc::RoutingTableCacheGossipMetadataHook>(
            operationContext()->getServiceContext());
    }

protected:
    std::unique_ptr<rpc::RoutingTableCacheGossipMetadataHook> gossipHook;
};

TEST_F(RoutingTableCacheGossipMetadataHookTest, writeRequestMetadata) {
    // Just noop.
    BSONObjBuilder bob;
    ASSERT_OK(gossipHook->writeRequestMetadata(operationContext(), &bob));
}

TEST_F(RoutingTableCacheGossipMetadataHookTest, readReplyMetadata) {
    // Read empty metadata.
    ASSERT_OK(gossipHook->readReplyMetadata(operationContext(), BSONObj()));

    // Read metadata with empty gossip.
    ASSERT_OK(gossipHook->readReplyMetadata(operationContext(), BSON("otherStuff" << 1)));
    ASSERT_OK(gossipHook->readReplyMetadata(
        operationContext(),
        BSON("otherStuff" << 1 << GenericReplyFields::kRoutingCacheGossipFieldName
                          << BSONArray())));

    // Read metadata with gossip info and check that the CatalogCache is notified.
    const GossipedRoutingCache gossip1(
        NamespaceString::createNamespaceString_forTest("test", "foo"),
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(5, 0)));
    const GossipedRoutingCache gossip2(
        NamespaceString::createNamespaceString_forTest("test", "bar"),
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(11, 0)},
                     CollectionPlacement(11, 0)));
    BSONArrayBuilder arrBuilder;
    arrBuilder.append(gossip1.toBSON());
    arrBuilder.append(gossip2.toBSON());
    ASSERT_OK(gossipHook->readReplyMetadata(
        operationContext(),
        BSON("otherStuff" << 1 << GenericReplyFields::kRoutingCacheGossipFieldName
                          << arrBuilder.arr())));

    ASSERT_EQ(gossip1.getCollectionVersion(),
              getCatalogCacheMock()->lastNotifiedTimeInStore[gossip1.getNss()]);
    ASSERT_EQ(gossip2.getCollectionVersion(),
              getCatalogCacheMock()->lastNotifiedTimeInStore[gossip2.getNss()]);


    // Read ill-formed gossip info. Should return error.
    ASSERT_NOT_OK(gossipHook->readReplyMetadata(
        operationContext(),
        BSON("otherStuff" << 1 << GenericReplyFields::kRoutingCacheGossipFieldName
                          << "unexpected")));
}

}  // namespace
}  // namespace mongo
