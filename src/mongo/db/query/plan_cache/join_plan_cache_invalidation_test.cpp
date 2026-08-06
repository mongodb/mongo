// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
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

    // Acquires 'nss' for read (MODE_IS). Used to snapshot/compare tags without holding the X lock,
    // so a subsequent real DDL (which takes its own X lock) can run.
    CollectionAcquisition acquireForRead(const NamespaceString& nss) {
        return acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
    }

    // CollectionVersionTag lives on the Collection decoration and can only be mutated through a
    // writable Collection*; a CollectionAcquisition's CollectionPtr is always const. This drives
    // the same production bump helper that real DDL operations use, wrapped in a WUOW on a writable
    // clone (mirroring how DDLs perform the bump).
    void bumpCatalogVersion(CollectionAcquisition& acquisition) {
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer(operationContext(), &acquisition);
        join_ordering::bumpCollectionVersionForDDL(
            writer.getWritableCollection(operationContext()));
        wuow.commit();
    }
};

TEST_F(JoinPlanCacheInvalidationTest, MakeCollectionTagsCapturesCurrentTags) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    // makeCollectionTags snapshots the collection's live version tag. Note that a freshly created
    // collection is not necessarily at version 0: building its _id index is itself a DDL that bumps
    // the version. So compare against the live value rather than a hardcoded 0.
    const auto& liveTag = JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get());

    auto tags = makeCollectionTags(mca);
    ASSERT_EQ(1, tags.size());
    ASSERT_EQ(coll.uuid(), tags[0].uuid);
    ASSERT_EQ(liveTag.collectionVersion, tags[0].versionTag.collectionVersion);
    ASSERT_EQ(0, tags[0].versionTag.sampleVersion);

    auto entry = std::make_unique<JoinPlanCacheEntry>(
        nullptr, join_ordering::NodeId{0}, tags, std::vector<NodeFingerprint>{});
    ASSERT_EQ(1, entry->collections.size());
    ASSERT_EQ(coll.uuid(), entry->collections[0].uuid);
    ASSERT_EQ(liveTag.collectionVersion, entry->collections[0].versionTag.collectionVersion);
    ASSERT_EQ(0, entry->collections[0].versionTag.sampleVersion);
}

TEST_F(JoinPlanCacheInvalidationTest, TagsAreCurrentWhenNothingChanged) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto tags = makeCollectionTags(mca);
    ASSERT_EQ(CollectionTagStatus::kCurrent, classifyCollectionTags(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, TagsNeedRevalidationAfterSimulatedCatalogChange) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto tags = makeCollectionTags(mca);
    const auto baseVersion = tags[0].versionTag.collectionVersion;

    bumpCatalogVersion(coll);
    ASSERT_EQ(baseVersion + 1,
              JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion);

    // The DDL may have been irrelevant to any cached plan, so this is the recoverable status
    // rather than an outright rejection.
    ASSERT_EQ(CollectionTagStatus::kNeedsIndexRevalidation, classifyCollectionTags(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, TagsNeedRevalidationWhenCachedTagIsAheadOfCurrent) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    // Simulate a cached tag whose version is ahead of the collection's actual current version
    // (the reverse of TagsAreNotCurrentAfterSimulatedCatalogChange, where current is ahead of
    // cached). The comparison is plain equality, so either direction of mismatch must be treated
    // as invalidated -- there's no "cached is still older, so it's fine" special case.
    std::vector<CollectionTag> tags{CollectionTag{
        coll.uuid(), CollectionVersionTag{.collectionVersion = 5, .sampleVersion = 0}}};

    ASSERT_EQ(CollectionTagStatus::kNeedsIndexRevalidation, classifyCollectionTags(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, TagsAreStaleWhenCollectionIsGone) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto tags = makeCollectionTags(mca);

    // 'mca' below has no collection matching the tag's uuid (simulating the referenced collection
    // having been dropped/renamed since the plan was cached). This must not crash, and there is no
    // catalog left to fingerprint against, so it must not ask for revalidation either.
    MultipleCollectionAccessor emptyMca;
    ASSERT_EQ(CollectionTagStatus::kStale, classifyCollectionTags(tags, emptyMca));
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
    ASSERT_EQ(CollectionTagStatus::kCurrent, classifyCollectionTags(tags, mca));

    // Bumping only the secondary collection's tag should be enough to invalidate.
    bumpCatalogVersion(secondaryColl);
    ASSERT_EQ(CollectionTagStatus::kNeedsIndexRevalidation, classifyCollectionTags(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, ClassifyTagsReportsMostRestrictiveStatusAcrossCollections) {
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

    // One collection revalidatable, the other gone: the entry must be rejected outright rather
    // than given the fingerprint second chance.
    ++tags[0].versionTag.collectionVersion;
    tags[1].uuid = UUID::gen();
    ASSERT_EQ(CollectionTagStatus::kStale, classifyCollectionTags(tags, mca));
}

TEST_F(JoinPlanCacheInvalidationTest, BumpCollectionVersionForDDLIncrementsVersion) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    const auto baseVersion =
        JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion;

    bumpCatalogVersion(coll);
    ASSERT_EQ(baseVersion + 1,
              JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion);
}

TEST_F(JoinPlanCacheInvalidationTest, BumpCollectionVersionForDDLIsMonotonicAcrossSuccessiveDDLs) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    const auto baseVersion =
        JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion;

    // Successive DDLs must yield strictly increasing versions (base -> base+1 -> base+2), NOT the
    // same value twice.
    bumpCatalogVersion(coll);
    ASSERT_EQ(baseVersion + 1,
              JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion);

    bumpCatalogVersion(coll);
    ASSERT_EQ(baseVersion + 2,
              JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion);
}

TEST_F(JoinPlanCacheInvalidationTest, RealIndexCreationBumpsVersionAndRequiresRevalidation) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));

    // Snapshot tags against the pre-DDL collection state, then release the lock so the index build
    // can take its own X lock. The baseline version is whatever collection creation left it at
    // (creating the _id index is itself a version-bumping DDL), so capture it rather than assume 0.
    std::vector<CollectionTag> tags;
    uint64_t baseVersion = 0;
    {
        auto coll = acquireForRead(nss);
        MultipleCollectionAccessor mca(coll);
        tags = makeCollectionTags(mca);
        ASSERT_EQ(1, tags.size());
        baseVersion = tags[0].versionTag.collectionVersion;
    }

    // Perform an index-creation DDL. This exercises the production bump path (via
    // multi_index_block) end to end.
    ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
        operationContext(), nss, {BSON("v" << 2 << "name" << "a_1" << "key" << BSON("a" << 1))}));


    auto coll = acquireForRead(nss);
    MultipleCollectionAccessor mca(coll);
    // The DDL bumped the live version past the captured baseline, so the previously-captured
    // tags no longer validate on their own.
    ASSERT_LT(baseVersion,
              JoinPlanCache::currentVersionTags(coll.getCollectionPtr().get()).collectionVersion);
    ASSERT_EQ(CollectionTagStatus::kNeedsIndexRevalidation, classifyCollectionTags(tags, mca));
}

}  // namespace
}  // namespace mongo
