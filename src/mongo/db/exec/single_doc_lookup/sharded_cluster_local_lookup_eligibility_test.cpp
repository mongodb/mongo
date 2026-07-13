// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/sharded_cluster_local_lookup_eligibility.h"

#include "mongo/db/keypattern.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_mock.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <memory>
#include <string_view>

namespace mongo::exec::agg {
namespace {

using Local = LocalLookupEligibility::Local;
using Decision = LocalLookupEligibility::Decision;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db", "coll");
const ShardId kThisShard{"shard0"};
const ShardId kOtherShard{"shard1"};

// Wraps a CollectionMetadata so a ScopedCollectionFilter can be built directly from a CRI, without
// a CollectionShardingState / acquisition.
class TestScopedCollectionFilterImpl : public ScopedCollectionFilter::Impl {
public:
    explicit TestScopedCollectionFilterImpl(CollectionMetadata metadata)
        : _metadata(std::move(metadata)) {}

    const CollectionMetadata& get() override {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
};

// Two entry points, mirroring how run() obtains each input:
//   - decideFromCri(): the NoHeldAcquisition arm self-fetches the CRI via route()/Grid, so it
//     cannot be injected through run(); call the pure decision directly with a mock CRI.
//   - decideFromHeldFilter(): the held arms receive the acquisition through AcquisitionState, so
//     drive the real run() and let it dispatch.
// Each sharded scenario asserts both: same locality, but the routing path returns a version to
// install while the held path reuses the already-versioned acquisition (no version).
class ShardedEligibilityTest : public unittest::Test {
protected:
    boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
        return make_intrusive<ExpressionContextForTest>(_opCtx.get(), kNss);
    }

    Decision decideFromCri(
        const CollectionRoutingInfo& cri,
        const Document& documentKey,
        const boost::optional<LogicalTime>& placementConflictTime = boost::none) {
        return ShardedClusterLocalLookupEligibility::decideFromCri(
            makeExpCtx(), cri, documentKey, kThisShard, placementConflictTime);
    }

    // Drives the held-sharded arm through the public run(): the filter stands in for the one
    // production takes from the held acquisition, not from the routing CRI.
    Decision decideFromHeldFilter(const CollectionRoutingInfo& cri, const Document& documentKey) {
        auto filter = ScopedCollectionFilter(std::make_shared<TestScopedCollectionFilterImpl>(
            CollectionMetadata(cri.getCurrentChunkManager(), kThisShard)));
        return runDecision(LocalLookupEligibility::HeldShardedCollection{std::cref(filter)},
                           documentKey);
    }

    // Drives run() with the given AcquisitionState and returns the Decision the body observed.
    Decision runDecision(const LocalLookupEligibility::AcquisitionState& acquisitionState,
                         const Document& documentKey) {
        ShardedClusterLocalLookupEligibility eligibility{kThisShard};
        boost::optional<Decision> captured;
        eligibility.run(makeExpCtx(),
                        kNss,
                        documentKey,
                        acquisitionState,
                        [&](const Decision& d) -> SingleDocumentLookupExecutor::LookupResult {
                            captured = d;
                            return {};
                        });
        return *captured;
    }

    static DatabaseVersion dbVersion() {
        return DatabaseVersion{UUID::gen(), Timestamp(1, 1)};
    }

    // A sharded collection with a single chunk [MINKEY, MAXKEY) on 'owner', shard key {_id: 1}.
    CollectionRoutingInfo shardedOnId(const ShardId& owner) {
        return CatalogCacheMock::makeCollectionRoutingInfoSharded(
            kNss,
            kThisShard /* dbPrimary */,
            dbVersion(),
            KeyPattern(BSON("_id" << 1)),
            {{ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)), owner}});
    }

    // A sharded collection on 'shardKey', split [MINKEY, 0) -> us, [0, MAXKEY) -> other.
    CollectionRoutingInfo splitOn(std::string_view shardKey) {
        return CatalogCacheMock::makeCollectionRoutingInfoSharded(
            kNss,
            kThisShard,
            dbVersion(),
            KeyPattern(BSON(shardKey << 1)),
            {{ChunkRange(BSON(shardKey << MINKEY), BSON(shardKey << 0)), kThisShard},
             {ChunkRange(BSON(shardKey << 0), BSON(shardKey << MAXKEY)), kOtherShard}});
    }

    QueryTestServiceContext _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx = _serviceContext.makeOperationContext();
};

// Both decision arms resolve locality from the documentKey's shard key, so a Local decision
// confirms ownership and the executor may skip the redundant post-read shard filter.
TEST_F(ShardedEligibilityTest, ChecksShardKeyOwnership) {
    ASSERT_TRUE(ShardedClusterLocalLookupEligibility{kThisShard}.checksShardKeyOwnership());
}

TEST_F(ShardedEligibilityTest, ShardedKeyOwnedByUs) {
    auto cri = shardedOnId(kThisShard);
    const Document key{{"_id", 1}};

    // Routing path: Local carrying a shard version to install, no db version.
    auto routed = decideFromCri(cri, key);
    ASSERT_TRUE(LocalLookupEligibility::isLocal(routed));
    ASSERT_TRUE(std::get<Local>(routed).shardVersion.has_value());
    ASSERT_FALSE(std::get<Local>(routed).dbVersion.has_value());

    // Held path: Local with nothing to install.
    auto held = decideFromHeldFilter(cri, key);
    ASSERT_TRUE(LocalLookupEligibility::isLocal(held));
    ASSERT_FALSE(std::get<Local>(held).shardVersion.has_value());
    ASSERT_FALSE(std::get<Local>(held).dbVersion.has_value());
}

TEST_F(ShardedEligibilityTest, ShardedKeyOwnedByOther) {
    auto cri = shardedOnId(kOtherShard);
    const Document key{{"_id", 1}};

    ASSERT_FALSE(LocalLookupEligibility::isLocal(decideFromCri(cri, key)));
    ASSERT_FALSE(LocalLookupEligibility::isLocal(decideFromHeldFilter(cri, key)));
}

TEST_F(ShardedEligibilityTest, IdShardKeySplitLocalityDependsOnLookupValue) {
    auto cri = splitOn("_id");

    // _id in our chunk: local on both paths (routing installs a version, held does not).
    auto routedLocal = decideFromCri(cri, Document{{"_id", -5}});
    ASSERT_TRUE(LocalLookupEligibility::isLocal(routedLocal));
    ASSERT_TRUE(std::get<Local>(routedLocal).shardVersion.has_value());
    ASSERT_FALSE(std::get<Local>(routedLocal).dbVersion.has_value());

    auto heldLocal = decideFromHeldFilter(cri, Document{{"_id", -5}});
    ASSERT_TRUE(LocalLookupEligibility::isLocal(heldLocal));
    ASSERT_FALSE(std::get<Local>(heldLocal).shardVersion.has_value());

    // _id in the other shard's chunk: not local on either path.
    ASSERT_FALSE(LocalLookupEligibility::isLocal(decideFromCri(cri, Document{{"_id", 5}})));
    ASSERT_FALSE(LocalLookupEligibility::isLocal(decideFromHeldFilter(cri, Document{{"_id", 5}})));
}

TEST_F(ShardedEligibilityTest, NonIdShardKeySplitLocalityDependsOnLookupValue) {
    auto cri = splitOn("sk");

    auto routedLocal = decideFromCri(cri, Document{{"_id", 1}, {"sk", -5}});
    ASSERT_TRUE(LocalLookupEligibility::isLocal(routedLocal));
    ASSERT_TRUE(std::get<Local>(routedLocal).shardVersion.has_value());
    ASSERT_FALSE(std::get<Local>(routedLocal).dbVersion.has_value());

    auto heldLocal = decideFromHeldFilter(cri, Document{{"_id", 1}, {"sk", -5}});
    ASSERT_TRUE(LocalLookupEligibility::isLocal(heldLocal));
    ASSERT_FALSE(std::get<Local>(heldLocal).shardVersion.has_value());

    ASSERT_FALSE(
        LocalLookupEligibility::isLocal(decideFromCri(cri, Document{{"_id", 1}, {"sk", 5}})));
    ASSERT_FALSE(LocalLookupEligibility::isLocal(
        decideFromHeldFilter(cri, Document{{"_id", 1}, {"sk", 5}})));
}

TEST_F(ShardedEligibilityTest, DocumentKeyNotCoveringShardKeyStraddlesShardsReturnsUnknown) {
    // Routing path only: a documentKey that does not constrain the shard key targets all shards,
    // so it is not provably local. The held arm is never reached with a partial key in practice
    // (a change-event documentKey always carries the full shard key), so it is not exercised here.
    auto cri = splitOn("sk");
    ASSERT_FALSE(LocalLookupEligibility::isLocal(decideFromCri(cri, Document{{"_id", 1}})));
}

TEST_F(ShardedEligibilityTest, PlacementConflictTimeIsStampedOntoLocalShardVersion) {
    // Inside a transaction the bundled version must carry the placementConflictTime, forwarded
    // verbatim to resolveShardRoleVersions. Routing path only; the held path installs no version.
    const auto placementConflictTime = LogicalTime(Timestamp(1000, 0));
    auto decision =
        decideFromCri(shardedOnId(kThisShard), Document{{"_id", 1}}, placementConflictTime);

    ASSERT_TRUE(LocalLookupEligibility::isLocal(decision));
    const auto& local = std::get<Local>(decision);
    ASSERT_TRUE(local.shardVersion.has_value());
    ASSERT_EQ(placementConflictTime, local.shardVersion->placementConflictTime_DEPRECATED());
}

TEST_F(ShardedEligibilityTest,
       UntrackedCollectionPrimaryIsUsReturnsLocalWithUntrackedAndDbVersion) {
    auto cri = CatalogCacheMock::makeCollectionRoutingInfoUntracked(kNss, kThisShard, dbVersion());
    auto decision = decideFromCri(cri, Document{{"_id", 1}});

    ASSERT_TRUE(LocalLookupEligibility::isLocal(decision));
    const auto& local = std::get<Local>(decision);

    // Tracked-unsharded convention: explicit UNTRACKED shard version plus the db version.
    ASSERT_TRUE(local.shardVersion.has_value());
    ASSERT_TRUE(local.dbVersion.has_value());
}

TEST_F(ShardedEligibilityTest, UntrackedCollectionPrimaryIsOtherReturnsUnknown) {
    auto cri = CatalogCacheMock::makeCollectionRoutingInfoUntracked(kNss, kOtherShard, dbVersion());
    ASSERT_FALSE(LocalLookupEligibility::isLocal(decideFromCri(cri, Document{{"_id", 1}})));
}

TEST_F(ShardedEligibilityTest, HeldUnshardedCollectionReturnsLocalWithoutVersion) {
    auto decision =
        runDecision(LocalLookupEligibility::HeldUnshardedCollectionLocally{}, Document{{"_id", 1}});

    ASSERT_TRUE(LocalLookupEligibility::isLocal(decision));
    const auto& local = std::get<Local>(decision);

    // Reused acquisition: nothing for the caller to install.
    ASSERT_FALSE(local.shardVersion.has_value());
    ASSERT_FALSE(local.dbVersion.has_value());
}

using LookupResult = SingleDocumentLookupExecutor::LookupResult;

// Integration coverage for run(): exercises the real CollectionRouter::route() against a
// CatalogCacheMock installed in Grid, on a shard-server fixture so run()'s
// shard_role_loop::withStaleShardRetry (and its CollectionShardingState access) runs for real.
// Proves run() fetches the routed CollectionRoutingInfo, derives the Decision from it, invokes the
// body, and (on a CAR error thrown by the body) lets the shard-role loop / route() drive the
// appropriate refresh + retry. The local shard is the fixture's own 'kMyShardName'.
class ShardedEligibilityRouteTest : public ShardServerTestFixtureWithCatalogCacheMock {
protected:
    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheMock::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        Grid::get(operationContext())->setShardingInitialized();
    }

    boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
        return make_intrusive<ExpressionContextForTest>(operationContext(), kNss);
    }

    static DatabaseVersion dbVersion() {
        return DatabaseVersion{UUID::gen(), Timestamp(1, 1)};
    }

    CollectionRoutingInfo shardedOnId(const ShardId& owner) {
        return CatalogCacheMock::makeCollectionRoutingInfoSharded(
            kNss,
            kMyShardName /* dbPrimary */,
            dbVersion(),
            KeyPattern(BSON("_id" << 1)),
            {{ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)), owner}});
    }

    ShardedClusterLocalLookupEligibility makeEligibility() {
        return ShardedClusterLocalLookupEligibility(kMyShardName);
    }

    static LookupResult found() {
        return {LookupResult::HandledStatus::kDocumentFound, Document{{"_id", 1}}};
    }
    static LookupResult notHandled() {
        return {LookupResult::HandledStatus::kNotHandled, boost::none};
    }
};

TEST_F(ShardedEligibilityRouteTest, RunRoutesAgainstGridAndDerivesLocalDecision) {
    getCatalogCacheMock()->setCollectionReturnValue(kNss, shardedOnId(kMyShardName));
    auto eligibility = makeEligibility();

    int calls = 0;
    bool sawLocal = false;
    auto result = eligibility.run(makeExpCtx(),
                                  kNss,
                                  Document{{"_id", 1}},
                                  LocalLookupEligibility::NoHeldAcquisition{},
                                  [&](const Decision& decision) {
                                      ++calls;
                                      sawLocal = LocalLookupEligibility::isLocal(decision);
                                      return found();
                                  });

    ASSERT_EQ(calls, 1);
    ASSERT_TRUE(sawLocal);
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
}

TEST_F(ShardedEligibilityRouteTest, RunRoutesAndDerivesUnknownForForeignOwner) {
    getCatalogCacheMock()->setCollectionReturnValue(kNss, shardedOnId(kOtherShard));
    auto eligibility = makeEligibility();

    bool sawLocal = true;
    auto result = eligibility.run(makeExpCtx(),
                                  kNss,
                                  Document{{"_id", 1}},
                                  LocalLookupEligibility::NoHeldAcquisition{},
                                  [&](const Decision& decision) {
                                      sawLocal = LocalLookupEligibility::isLocal(decision);
                                      return notHandled();
                                  });

    ASSERT_FALSE(sawLocal);
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kNotHandled);
}

// A *routing*-stale StaleConfig thrown by the body must flow through the shard-role loop (which
// recognizes it as router-stale and rethrows) up to route(), which refreshes the catalog and
// re-runs the body against the now-current routing info. The body swaps the mock's return value on
// its first invocation (simulating the post-refresh fresh CRI) before throwing.
//
// The error is shaped router-stale (versionWanted > versionReceived) precisely so the shard-role
// loop classifies it as "router is stale", leaves the stale-shard handler untouched, and rethrows
// to route().
TEST_F(ShardedEligibilityRouteTest, StaleConfigFromBodyDrivesRefreshAndRetry) {
    // Start with foreign-owned routing so the first decision is Unknown. On its first invocation
    // the body swaps the mock to this-shard-owned routing and throws StaleConfig; route() refreshes
    // and re-runs the body against the now-local routing, flipping the decision to Local. Asserting
    // the flip proves the retry observed the refreshed CollectionRoutingInfo, not a stale cache.
    getCatalogCacheMock()->setCollectionReturnValue(kNss, shardedOnId(kOtherShard));
    auto eligibility = makeEligibility();

    // Router-stale: the shard reports a placement version newer than the one the router sent, so
    // shard_role_loop classifies it as "router is stale" and rethrows to route().
    const auto generation = CollectionGeneration(OID::gen(), Timestamp(1, 0));
    const auto versionReceived = ShardVersionFactory::make(ChunkVersion(generation, {1, 0}));
    const auto versionWanted = ShardVersionFactory::make(ChunkVersion(generation, {2, 0}));

    int calls = 0;
    bool firstDecisionLocal = false;
    bool secondDecisionLocal = false;
    auto result = eligibility.run(
        makeExpCtx(),
        kNss,
        Document{{"_id", 1}},
        LocalLookupEligibility::NoHeldAcquisition{},
        [&](const Decision& decision) -> LookupResult {
            ++calls;
            const bool decisionIsLocal = LocalLookupEligibility::isLocal(decision);
            if (calls == 1) {
                firstDecisionLocal = decisionIsLocal;
                getCatalogCacheMock()->setCollectionReturnValue(kNss, shardedOnId(kMyShardName));
                uasserted(StaleConfigInfo(kNss, versionReceived, versionWanted, kMyShardName),
                          "router-stale config from local read");
            }
            secondDecisionLocal = decisionIsLocal;
            return found();
        });

    ASSERT_EQ(calls, 2);
    ASSERT_FALSE(firstDecisionLocal);  // pre-refresh: foreign-owned -> Unknown
    ASSERT_TRUE(secondDecisionLocal);  // post-refresh: this-shard-owned -> Local
    ASSERT_EQ(result.status, LookupResult::HandledStatus::kDocumentFound);
}

// Note: the *shard*-stale path (the shard-role loop recovers the filtering metadata locally and
// retries the body in place, without escaping to route() for a routing refresh) is unit-tested in
// shard_role_loop_test, which mocks the metadata recovery. Reproducing it here would invoke the
// real shard recovery against a mock catalog, so it is intentionally not duplicated.

}  // namespace
}  // namespace mongo::exec::agg
