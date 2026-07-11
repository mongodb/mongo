/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/s/client_metadata_propagation_egress_hook.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/ifr_sender_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/version/releases.h"

#include <map>
#include <mutex>

namespace mongo {
namespace {

using namespace std::string_view_literals;

// A flag introduced at kLatest, dedicated to this test file, so `getFlagsIntroducedSinceLastLTS()`
// has something to serialize without coupling this test to the lifecycle of real product flags
// (e.g. featureFlagExtensionsInsideHybridSearch), which may be renamed or tombstoned independently
// of this egress hook logic.
void ensureTestIfrFlagRegistered() {
    // (Generic FCV reference): test usage
    static IncrementalRolloutFeatureFlag flag{
        "featureFlagIfrEgressHookTest"sv,
        RolloutPhase::inDevelopment,
        /*value=*/true,
        FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(
            multiversion::GenericFCV::kLatest)};
    static std::once_flag registered;
    std::call_once(registered, [] { flag.registerFlag(); });
}

/**
 * Fixture that scopes `serverGlobalParams.clusterRole` and the process-wide FCV so tests can
 * exercise router-vs-shard branches of the egress hook hermetically.
 */
class ClientMetadataPropagationEgressHookTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _savedRole = serverGlobalParams.clusterRole;
        _savedFcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    }

    void tearDown() override {
        serverGlobalParams.mutableFCV.setVersion(_savedFcv);
        serverGlobalParams.clusterRole = _savedRole;
        ServiceContextTest::tearDown();
    }

    void setRouterOnly() {
        serverGlobalParams.clusterRole = ClusterRole::RouterServer;
    }

    void setShardOnly() {
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    }

    void setFcv(multiversion::FeatureCompatibilityVersion v) {
        serverGlobalParams.mutableFCV.setVersion(v);
    }

    rpc::ClientMetadataPropagationEgressHook hook;

private:
    ClusterRole _savedRole;
    // (Generic FCV reference): default FCV for the test fixture, restored in tearDown.
    multiversion::FeatureCompatibilityVersion _savedFcv{multiversion::GenericFCV::kLatest};
};

TEST_F(ClientMetadataPropagationEgressHookTest, NullOpCtxReturnsOkAndEmptyMetadata) {
    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(nullptr, &bob));
    ASSERT_TRUE(bob.obj().isEmpty());
}

TEST_F(ClientMetadataPropagationEgressHookTest, NoIFRContextInstalledEmitsNothing) {
    setRouterOnly();
    auto opCtx = makeOperationContext();
    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob));
    auto obj = bob.obj();
    ASSERT_FALSE(obj.hasField("ifrFlags"));
    ASSERT_FALSE(obj.hasField("ifrSenderVersion"));
}

TEST_F(ClientMetadataPropagationEgressHookTest, ShardNonWireOriginEmitsNothing) {
    setShardOnly();
    auto opCtx = makeOperationContext();
    // The shard-default context is installed lazily and left unmaterialized here (no flag is
    // consulted), so `tryGet()` reports no context and the hook appends nothing.
    IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(opCtx.get());
    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob));
    auto obj = bob.obj();
    ASSERT_FALSE(obj.hasField("ifrFlags"));
    ASSERT_FALSE(obj.hasField("ifrSenderVersion"));
}

TEST_F(ClientMetadataPropagationEgressHookTest,
       ShardNonWireOriginEmitsNothingAfterMaterialization) {
    setShardOnly();
    ensureTestIfrFlagRegistered();
    auto opCtx = makeOperationContext();
    IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(opCtx.get());
    // Force the deferred shard-default context to materialize, as a local flag consult would. The
    // deferral is egress-neutral: a shard that received no ifrFlags forwards nothing downstream
    // whether or not it happened to consult a flag locally, so materialization must not leak into
    // the egress payload.
    auto ctx = IncrementalFeatureRolloutContext::get(opCtx.get());
    ASSERT_TRUE(ctx);
    ASSERT_TRUE(IncrementalFeatureRolloutContext::isInstalled(opCtx.get()));
    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob));
    auto obj = bob.obj();
    ASSERT_FALSE(obj.hasField("ifrFlags")) << obj;
    ASSERT_FALSE(obj.hasField("ifrSenderVersion")) << obj;
}

TEST_F(ClientMetadataPropagationEgressHookTest, RouterModeStampsOwnFcvAndFlags) {
    setRouterOnly();
    ensureTestIfrFlagRegistered();

    // All flags introduced at kLatest are expected in the egress payload when the router sends at
    // its own version.
    const auto& expectedFlags = IncrementalRolloutFeatureFlag::getFlagsIntroducedSinceLastLTS();
    ASSERT_FALSE(expectedFlags.empty());

    auto opCtx = makeOperationContext();
    // Router installs a non-wire IFRContext (fresh request).
    IncrementalFeatureRolloutContext::set(opCtx.get(),
                                          std::make_shared<IncrementalFeatureRolloutContext>());
    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob));
    auto obj = bob.obj();

    ASSERT_TRUE(obj.hasField("ifrSenderVersion")) << obj;
    ASSERT_BSONOBJ_EQ(obj["ifrSenderVersion"].Obj(), makeLocalIFRSenderVersion().toBSON());

    ASSERT_TRUE(obj.hasField("ifrFlags")) << obj;
    std::map<std::string, bool> observed;
    for (const auto& el : obj["ifrFlags"].Array()) {
        auto sub = el.embeddedObject();
        observed[sub["name"].String()] = sub["value"].Bool();
    }
    for (auto* flag : expectedFlags) {
        ASSERT_TRUE(observed.find(flag->getName()) != observed.end())
            << "expected flag " << flag->getName();
    }
}

TEST_F(ClientMetadataPropagationEgressHookTest, ShardWireInstalledForwardsSenderVerbatim) {
    setShardOnly();
    // Even if local FCV is kLatest, wire-installed shard forwards the original sender's version.
    // (Generic FCV reference): test usage
    setFcv(multiversion::GenericFCV::kLatest);
    ensureTestIfrFlagRegistered();

    std::vector<BSONObj> emptyFlags;
    // Fabricate a distinct sender version so the test can prove byte-identical forwarding.
    // Snapshot its BSON before transferring ownership into fromWire, since the unique_ptr is
    // moved-from below.
    auto fabricatedSender = std::make_unique<IFRSenderVersion>();
    fabricatedSender->setMajor(9);
    fabricatedSender->setMinor(0);
    fabricatedSender->setPatch(3);
    fabricatedSender->setExtra(-4);
    const auto fabricatedSenderBson = fabricatedSender->toBSON();
    auto ifrCtx =
        IncrementalFeatureRolloutContext::fromWire(emptyFlags, std::move(fabricatedSender));
    auto opCtx = makeOperationContext();
    IncrementalFeatureRolloutContext::set(opCtx.get(), ifrCtx);

    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob));
    auto obj = bob.obj();

    ASSERT_TRUE(obj.hasField("ifrSenderVersion")) << obj;
    ASSERT_BSONOBJ_EQ(obj["ifrSenderVersion"].Obj(), fabricatedSenderBson);
    // The ifrFlags array is emitted unconditionally.
    ASSERT_TRUE(obj.hasField("ifrFlags")) << obj;
}

TEST_F(ClientMetadataPropagationEgressHookTest,
       ShardWireInstalledKLastLtsSenderExcludesLatestFlags) {
    // A wire-installed context whose sender is at kLastLTS must not forward kLatest-introduced
    // flags on the outgoing hop.
    setShardOnly();
    ensureTestIfrFlagRegistered();

    // (Generic FCV reference): construct a sender version at kLastLTS.
    auto senderVersion = std::make_unique<IFRSenderVersion>();
    senderVersion->setMajor(multiversion::majorVersion(multiversion::GenericFCV::kLastLTS));
    senderVersion->setMinor(multiversion::minorVersion(multiversion::GenericFCV::kLastLTS));
    senderVersion->setPatch(0);
    senderVersion->setExtra(0);

    std::vector<BSONObj> emptyFlags;
    auto ifrCtx = IncrementalFeatureRolloutContext::fromWire(emptyFlags, std::move(senderVersion));
    auto opCtx = makeOperationContext();
    IncrementalFeatureRolloutContext::set(opCtx.get(), ifrCtx);

    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob));
    auto obj = bob.obj();

    // The ifrFlags array is present (subarrayStart emits the field unconditionally), but must
    // not contain any flag introduced at kLatest.
    ASSERT_TRUE(obj.hasField("ifrFlags")) << obj;
    for (const auto& el : obj["ifrFlags"].Array()) {
        auto sub = el.embeddedObject();
        ASSERT_NE(sub["name"].String(), "featureFlagIfrEgressHookTest") << obj;
    }
}

TEST_F(ClientMetadataPropagationEgressHookTest, ShardWireInstalledMissingSenderOmitsField) {
    setShardOnly();
    ensureTestIfrFlagRegistered();

    std::vector<BSONObj> emptyFlags;
    auto ifrCtx = IncrementalFeatureRolloutContext::fromWire(emptyFlags, /*senderVersion=*/nullptr);
    auto opCtx = makeOperationContext();
    IncrementalFeatureRolloutContext::set(opCtx.get(), ifrCtx);

    BSONObjBuilder bob;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob));
    auto obj = bob.obj();

    ASSERT_FALSE(obj.hasField("ifrSenderVersion")) << obj;
    // The ifrFlags array is emitted unconditionally.
    ASSERT_TRUE(obj.hasField("ifrFlags")) << obj;
}

TEST_F(ClientMetadataPropagationEgressHookTest, CachingIsIdempotent) {
    setRouterOnly();
    // (Generic FCV reference): test usage
    setFcv(multiversion::GenericFCV::kLatest);
    ensureTestIfrFlagRegistered();

    auto opCtx = makeOperationContext();
    IncrementalFeatureRolloutContext::set(opCtx.get(),
                                          std::make_shared<IncrementalFeatureRolloutContext>());

    BSONObjBuilder bob1;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob1));
    auto obj1 = bob1.obj();

    BSONObjBuilder bob2;
    ASSERT_OK(hook.writeRequestMetadata(opCtx.get(), &bob2));
    auto obj2 = bob2.obj();

    // The IFR portions of the two builders must match byte-for-byte, proving the memoization
    // produced the same serialized sub-object on the second call.
    ASSERT_BSONOBJ_EQ(obj1.getField("ifrSenderVersion").wrap(),
                      obj2.getField("ifrSenderVersion").wrap());
    ASSERT_BSONOBJ_EQ(obj1.getField("ifrFlags").wrap(), obj2.getField("ifrFlags").wrap());
}

}  // namespace
}  // namespace mongo
