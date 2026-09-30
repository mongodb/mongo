// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/mock_transport_layer_with_mock_reactor.h"

#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/unittest.h"


namespace mongo::transport {
namespace {

TEST(MockTransportLayerWithMockReactorTest, HandsOutFreshEgressReactors) {
    MockTransportLayerWithMockReactor tl;

    // A TransportLayerManager selects a layer by protocol; egress consumers also check isEgress().
    ASSERT_TRUE(tl.isEgress());
    ASSERT_FALSE(tl.isIngress());
    ASSERT_EQ(tl.getTransportProtocol(), TransportProtocol::MongoRPC);
    ASSERT_EQ(tl.latestReactor(), nullptr);

    auto r1 = tl.getReactor(TransportLayer::kNewReactor);
    auto r2 = tl.getReactor(TransportLayer::kNewReactor);
    ASSERT_NE(r1, nullptr);
    ASSERT_NE(r2, nullptr);
    ASSERT_NE(r1, r2);
    ASSERT_EQ(tl.latestReactor(), r2);
    ASSERT_EQ(tl.reactors().size(), 2);

    // A stopped reactor stays stopped and does not affect the next one.
    r1->stop();
    ASSERT_TRUE(tl.reactors()[0]->isStopped());
    ASSERT_FALSE(tl.latestReactor()->isStopped());
}

TEST(MockTransportLayerWithMockReactorTest, OnlyNewReactorIsProvided) {
    MockTransportLayerWithMockReactor tl;

    ASSERT_EQ(tl.getReactor(TransportLayer::kIngress), nullptr);
    ASSERT_EQ(tl.getReactor(TransportLayer::kEgress), nullptr);
    ASSERT_EQ(tl.reactors().size(), 0);
    ASSERT_EQ(tl.latestReactor(), nullptr);

    auto fresh = tl.getReactor(TransportLayer::kNewReactor);
    ASSERT_NE(fresh, nullptr);
    ASSERT_EQ(tl.reactors().size(), 1);
    ASSERT_EQ(tl.latestReactor(), fresh);
}

}  // namespace
}  // namespace mongo::transport
