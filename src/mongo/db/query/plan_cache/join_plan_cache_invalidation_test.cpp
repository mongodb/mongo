// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class JoinPlanCacheInvalidationTest : public CatalogTestFixture {
public:
    CollectionAcquisition createAndAcquireCollection(const NamespaceString& nss) {
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
        return acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
    }

    // CollectionVersionTag lives on the Collection decoration and can only be mutated through a
    // writable Collection*; a CollectionAcquisition's CollectionPtr is always const. No production
    // code bumps this yet so tests simulate a DDL change directly.
    // TODO (SERVER-129267): See if we can remove this function once DDLs bump version tags.
    void bumpCatalogVersion(CollectionAcquisition& acquisition) {
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer(operationContext(), &acquisition);
        JoinPlanCache::currentVersionTags(writer.getWritableCollection(operationContext()))
            .collectionVersion++;
        wuow.commit();
    }
};

TEST_F(JoinPlanCacheInvalidationTest, MakeCollectionTagsCapturesCurrentTags) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto tags = makeCollectionTags(mca);
    ASSERT_EQ(1, tags.size());
    ASSERT_EQ(coll.uuid(), tags[0].uuid);
    ASSERT_EQ(0, tags[0].versionTag.collectionVersion);
    ASSERT_EQ(0, tags[0].versionTag.sampleVersion);

    auto entry = std::make_unique<JoinPlanCacheEntry>(nullptr, join_ordering::NodeId{0}, tags);
    ASSERT_EQ(1, entry->collections.size());
    ASSERT_EQ(coll.uuid(), entry->collections[0].uuid);
    ASSERT_EQ(0, entry->collections[0].versionTag.collectionVersion);
    ASSERT_EQ(0, entry->collections[0].versionTag.sampleVersion);
}

TEST_F(JoinPlanCacheInvalidationTest, TagsAreCurrentWhenNothingChanged) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto tags = makeCollectionTags(mca);
    ASSERT_TRUE(areCollectionTagsCurrent(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, TagsAreNotCurrentAfterSimulatedCatalogChange) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto tags = makeCollectionTags(mca);
    ASSERT_EQ(0, tags[0].versionTag.collectionVersion);

    bumpCatalogVersion(coll);
    ASSERT_EQ(1,
              JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion);

    ASSERT_FALSE(areCollectionTagsCurrent(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, TagsAreNotCurrentWhenCachedTagIsAheadOfCurrent) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    // Simulate a cached tag whose version is ahead of the collection's actual current version
    // (the reverse of TagsAreNotCurrentAfterSimulatedCatalogChange, where current is ahead of
    // cached). The comparison is plain equality, so either direction of mismatch must be treated
    // as invalidated -- there's no "cached is still older, so it's fine" special case.
    std::vector<CollectionTag> tags{CollectionTag{
        coll.uuid(), CollectionVersionTag{.collectionVersion = 5, .sampleVersion = 0}}};

    ASSERT_FALSE(areCollectionTagsCurrent(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, TagsAreNotCurrentWhenCollectionIsGone) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto tags = makeCollectionTags(mca);

    // 'mca' below has no collection matching the tag's uuid (simulating the referenced collection
    // having been dropped/renamed since the plan was cached). This must not crash.
    MultipleCollectionAccessor emptyMca;
    ASSERT_FALSE(areCollectionTagsCurrent(tags, emptyMca));
}

TEST_F(JoinPlanCacheInvalidationTest, MultiCollectionTagsTrackMainAndSecondary) {
    auto mainNss = NamespaceString::createNamespaceString_forTest("TestDB", "MainColl");
    auto secondaryNss = NamespaceString::createNamespaceString_forTest("TestDB", "SecondaryColl");
    auto mainColl = createAndAcquireCollection(mainNss);
    auto secondaryColl = createAndAcquireCollection(secondaryNss);

    MultipleCollectionAccessor mca(
        CollectionOrViewAcquisition(CollectionAcquisition(mainColl)),
        makeAcquisitionMap(CollectionOrViewAcquisitions{
            CollectionOrViewAcquisition(CollectionAcquisition(secondaryColl))}),
        false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

    auto tags = makeCollectionTags(mca);
    ASSERT_EQ(2, tags.size());
    ASSERT_TRUE(areCollectionTagsCurrent(tags, mca));

    // Bumping only the secondary collection's tag should be enough to invalidate.
    bumpCatalogVersion(secondaryColl);
    ASSERT_FALSE(areCollectionTagsCurrent(tags, mca));
}

}  // namespace
}  // namespace mongo
