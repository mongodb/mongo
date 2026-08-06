// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/death_test.h"
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

using JoinPlanCacheInvalidationDeathTest = JoinPlanCacheInvalidationTest;

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
    auto entryTags = entry->getCollectionTags();
    ASSERT_EQ(1, entryTags.size());
    ASSERT_EQ(coll.uuid(), entryTags[0].uuid);
    ASSERT_EQ(liveTag.collectionVersion, entryTags[0].versionTag.collectionVersion);
    ASSERT_EQ(0, entryTags[0].versionTag.sampleVersion);
}

TEST_F(JoinPlanCacheInvalidationTest, RefreshCollectionTagsAdoptsTheCurrentCollectionState) {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto entry = std::make_unique<JoinPlanCacheEntry>(
        nullptr, join_ordering::NodeId{0}, makeCollectionTags(mca), std::vector<NodeFingerprint>{});
    const auto baseVersion = entry->getCollectionTags()[0].versionTag.collectionVersion;

    bumpCatalogVersion(coll);
    ASSERT_EQ(CollectionTagStatus::kNeedsIndexRevalidation,
              classifyCollectionTags(entry->getCollectionTags(), mca));

    // Simulates what a lookup does once the relevant-index fingerprints have revalidated the entry
    // against the post-DDL catalog: adopt that catalog's version tags so the next lookup takes the
    // fast path without fingerprinting again.
    entry->refreshCollectionTags(makeCollectionTags(mca));

    ASSERT_EQ(baseVersion + 1, entry->getCollectionTags()[0].versionTag.collectionVersion);
    ASSERT_EQ(CollectionTagStatus::kCurrent,
              classifyCollectionTags(entry->getCollectionTags(), mca));
}

DEATH_TEST_F(
    JoinPlanCacheInvalidationDeathTest,
    RefreshCollectionTagsRejectsADifferentCollectionCount,
    "Refreshed join plan cache collection tags must cover the same number of collections") {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto entry = std::make_unique<JoinPlanCacheEntry>(
        nullptr, join_ordering::NodeId{0}, makeCollectionTags(mca), std::vector<NodeFingerprint>{});

    // A refresh covering a different number of collections would mean the tags were snapshotted
    // from a different join graph than the one this entry was cached for.
    entry->refreshCollectionTags({});
}

TEST_F(JoinPlanCacheInvalidationTest, RefreshCollectionTagsNeverMovesAVersionBackwards) {
    // A refresh carrying an older version than the one already recorded must not undo it. In
    // production this arrives from an operation whose catalog snapshot predates another's, but the
    // guard depends only on the two values, so ordering them explicitly is enough to test it.
    auto uuid = UUID::gen();
    auto entry = std::make_unique<JoinPlanCacheEntry>(
        nullptr,
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{
            CollectionTag{uuid, CollectionVersionTag{.collectionVersion = 1, .sampleVersion = 0}}},
        std::vector<NodeFingerprint>{});

    entry->refreshCollectionTags(std::vector<CollectionTag>{
        CollectionTag{uuid, CollectionVersionTag{.collectionVersion = 6, .sampleVersion = 0}}});
    ASSERT_EQ(6, entry->getCollectionTags()[0].versionTag.collectionVersion);

    entry->refreshCollectionTags(std::vector<CollectionTag>{
        CollectionTag{uuid, CollectionVersionTag{.collectionVersion = 5, .sampleVersion = 0}}});
    ASSERT_EQ(6, entry->getCollectionTags()[0].versionTag.collectionVersion);
}

TEST_F(JoinPlanCacheInvalidationTest, RefreshCollectionTagsPairsUpTagsByUuid) {
    // 'makeCollectionTags' walks the secondary collections of an 'absl::flat_hash_map', whose
    // iteration order is randomized per table, so a refresh can legitimately arrive listing the
    // same collections in a different order than they were cached in. Each version must still land
    // on the collection it was read from.
    auto firstUuid = UUID::gen();
    auto secondUuid = UUID::gen();
    std::vector<CollectionTag> cachedTags{
        CollectionTag{firstUuid, CollectionVersionTag{.collectionVersion = 1, .sampleVersion = 0}},
        CollectionTag{secondUuid,
                      CollectionVersionTag{.collectionVersion = 2, .sampleVersion = 0}}};

    auto entry = std::make_unique<JoinPlanCacheEntry>(
        nullptr, join_ordering::NodeId{0}, cachedTags, std::vector<NodeFingerprint>{});

    // The same two collections, reversed, each with a bumped version.
    std::vector<CollectionTag> refreshedTags{
        CollectionTag{secondUuid,
                      CollectionVersionTag{.collectionVersion = 20, .sampleVersion = 0}},
        CollectionTag{firstUuid,
                      CollectionVersionTag{.collectionVersion = 10, .sampleVersion = 0}}};
    entry->refreshCollectionTags(refreshedTags);

    auto updated = entry->getCollectionTags();
    ASSERT_EQ(2, updated.size());
    for (const auto& tag : updated) {
        if (tag.uuid == firstUuid) {
            ASSERT_EQ(10, tag.versionTag.collectionVersion);
        } else {
            ASSERT_EQ(secondUuid, tag.uuid);
            ASSERT_EQ(20, tag.versionTag.collectionVersion);
        }
    }
}

DEATH_TEST_F(JoinPlanCacheInvalidationDeathTest,
             RefreshCollectionTagsRejectsADifferentCollection,
             "Refreshed join plan cache collection tag matches no cached collection") {
    auto nss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
    auto coll = createAndAcquireCollection(nss);
    MultipleCollectionAccessor mca(coll);

    auto entry = std::make_unique<JoinPlanCacheEntry>(
        nullptr, join_ordering::NodeId{0}, makeCollectionTags(mca), std::vector<NodeFingerprint>{});

    // Right number of tags, wrong collection: with nothing to pair the incoming tag up with, the
    // versions of one collection must never end up recorded against another.
    std::vector<CollectionTag> foreignTags{CollectionTag{
        UUID::gen(), CollectionVersionTag{.collectionVersion = 7, .sampleVersion = 0}}};
    entry->refreshCollectionTags(foreignTags);
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
