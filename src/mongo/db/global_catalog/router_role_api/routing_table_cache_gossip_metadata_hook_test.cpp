/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/router_role_api/routing_table_cache_gossip_metadata_hook.h"

#include "mongo/db/global_catalog/router_role_api/gossiped_routing_cache_gen.h"
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
