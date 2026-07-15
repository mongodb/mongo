// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/max_key_orphan_detection.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(MaxKeyOrphanDetectionTest, EmptyObjectIsNotGlobalMax) {
    ASSERT_FALSE(isGlobalMaxShardKey(BSONObj()));
}

TEST(MaxKeyOrphanDetectionTest, AllFieldsMaxKeyIsGlobalMax) {
    ASSERT_TRUE(isGlobalMaxShardKey(BSON("a" << MAXKEY)));
    ASSERT_TRUE(isGlobalMaxShardKey(BSON("a" << MAXKEY << "b" << MAXKEY)));
}

TEST(MaxKeyOrphanDetectionTest, PartialOrNonMaxKeyIsNotGlobalMax) {
    // Only a leading MaxKey field: the shard key is strictly below the global max, so it is not a
    // never-cloned orphan candidate.
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << MAXKEY << "b" << 10)));
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << MAXKEY << "b" << MINKEY)));
    // No leading MaxKey at all.
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << 5)));
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << 5 << "b" << MAXKEY)));
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << MINKEY << "b" << MAXKEY)));
}

constexpr std::string_view kScanStateIdValue = "scanState";
constexpr long long kStartingTerm = 1;

boost::optional<BSONObj> readMaxKeyOrphanScanStateDoc(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    FindCommandRequest findCmd(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace);
    findCmd.setFilter(BSON("_id" << kScanStateIdValue));
    findCmd.setLimit(1);
    auto cursor = client.find(std::move(findCmd));
    if (cursor && cursor->more()) {
        return cursor->next().getOwned();
    }
    return boost::none;
}

void clearMaxKeyOrphanScanStateDoc(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    client.remove(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace,
                  BSON("_id" << kScanStateIdValue));
}

// Overwrites the state doc with the requested flags so a subsequent sweep runs against a known
// starting state. A completed prior sweep is represented by the presence of scanCompletedAt; when
// 'scanComplete' is false the field is omitted to mimic an abandoned sweep.
void seedMaxKeyOrphanScanStateDoc(OperationContext* opCtx,
                                  bool scanComplete,
                                  bool foundUnownedMaxKey,
                                  bool unownedAlertEmitted,
                                  bool foundOwnedMaxKey = false,
                                  bool ownedAlertEmitted = false) {
    DBDirectClient client(opCtx);
    client.remove(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace,
                  BSON("_id" << kScanStateIdValue));
    BSONObjBuilder docBob;
    docBob.append("_id", kScanStateIdValue);
    docBob.append("scanStartedAt", Date_t::now());
    if (scanComplete) {
        docBob.append("scanCompletedAt", Date_t::now());
    }
    docBob.append("foundUnownedMaxKey", foundUnownedMaxKey);
    docBob.append("unownedAlertEmitted", unownedAlertEmitted);
    docBob.append("foundOwnedMaxKey", foundOwnedMaxKey);
    docBob.append("ownedAlertEmitted", ownedAlertEmitted);
    client.insert(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace, docBob.obj());
}

class MaxKeyOrphanDetectionFixture : service_context_test::WithSetupTransportLayer,
                                     public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        // The detector only persists once the node is a writable primary. ShardServerTestFixture
        // marks the node primary but leaves it non-writable, so without this the wait in
        // runMaxKeyOrphanDetection (under Intent Registration) would never make progress.
        replicationCoordinator()->setCanAcceptNonLocalWrites(true);
        opCtx = operationContext();
    }

    OperationContext* opCtx;

private:
    unittest::ServerParameterGuard _maxKeyDetectionFlag{"featureFlagMaxKeyDetection", true};
};

TEST_F(MaxKeyOrphanDetectionFixture, CleanClusterPersistsScanComplete) {
    runMaxKeyOrphanDetection(opCtx, kStartingTerm);

    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(doc.has_value()) << "Expected the scan state doc after the sweep";
    ASSERT(doc->hasField("scanCompletedAt")) << *doc;
    ASSERT_FALSE(doc->getField("foundUnownedMaxKey").Bool()) << *doc;
    ASSERT_FALSE(doc->getField("unownedAlertEmitted").Bool()) << *doc;
    ASSERT_FALSE(doc->getField("foundOwnedMaxKey").Bool()) << *doc;
    ASSERT_FALSE(doc->getField("ownedAlertEmitted").Bool()) << *doc;
    ASSERT(doc->hasField("scanStartedAt")) << *doc;
}

TEST_F(MaxKeyOrphanDetectionFixture, ShortCircuitsAfterScanComplete) {
    runMaxKeyOrphanDetection(opCtx, kStartingTerm);
    auto initial = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(initial.has_value());
    ASSERT(initial->hasField("scanCompletedAt"));

    runMaxKeyOrphanDetection(opCtx, kStartingTerm + 1);

    auto afterRerun = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(afterRerun.has_value());
    ASSERT_BSONOBJ_EQ(*initial, *afterRerun);
}

TEST_F(MaxKeyOrphanDetectionFixture, ReRunsAfterStateDocCleared) {
    runMaxKeyOrphanDetection(opCtx, kStartingTerm);
    clearMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT_FALSE(readMaxKeyOrphanScanStateDoc(opCtx).has_value());

    runMaxKeyOrphanDetection(opCtx, kStartingTerm + 1);

    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(doc.has_value());
    ASSERT(doc->hasField("scanCompletedAt")) << *doc;
    ASSERT_FALSE(doc->getField("foundUnownedMaxKey").Bool());
}

TEST_F(MaxKeyOrphanDetectionFixture, ReRunsWhenPriorScanIncomplete) {
    seedMaxKeyOrphanScanStateDoc(
        opCtx, /*scanComplete=*/false, /*foundUnownedMaxKey=*/false, /*unownedAlertEmitted=*/false);

    runMaxKeyOrphanDetection(opCtx, kStartingTerm);

    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(doc.has_value());
    ASSERT(doc->hasField("scanCompletedAt")) << *doc;
}

TEST_F(MaxKeyOrphanDetectionFixture, PreservesUnownedAlertEmittedAcrossRescan) {
    seedMaxKeyOrphanScanStateDoc(
        opCtx, /*scanComplete=*/false, /*foundUnownedMaxKey=*/false, /*unownedAlertEmitted=*/true);

    runMaxKeyOrphanDetection(opCtx, kStartingTerm);

    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(doc.has_value());
    ASSERT(doc->hasField("scanCompletedAt")) << *doc;
    ASSERT_TRUE(doc->getField("unownedAlertEmitted").Bool())
        << "Expected the re-scan to preserve a prior unownedAlertEmitted=true: " << *doc;
}

TEST_F(MaxKeyOrphanDetectionFixture, PreservesOwnedAlertEmittedAcrossRescan) {
    seedMaxKeyOrphanScanStateDoc(opCtx,
                                 /*scanComplete=*/false,
                                 /*foundUnownedMaxKey=*/false,
                                 /*unownedAlertEmitted=*/false,
                                 /*foundOwnedMaxKey=*/false,
                                 /*ownedAlertEmitted=*/true);

    runMaxKeyOrphanDetection(opCtx, kStartingTerm);

    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(doc.has_value());
    ASSERT(doc->hasField("scanCompletedAt")) << *doc;
    ASSERT_TRUE(doc->getField("ownedAlertEmitted").Bool())
        << "Expected the re-scan to preserve a prior ownedAlertEmitted=true: " << *doc;
}

TEST(MaxKeyOrphanDetectionTest, DetectionGateHonorsServerParameterAndFeatureFlag) {
    ASSERT_FALSE(sharding_util::isMaxKeyDetectionEnabled()) << "both toggles default off";

    {
        unittest::ServerParameterGuard paramOn{"enableMaxKeyDetection", true};
        ASSERT_TRUE(sharding_util::isMaxKeyDetectionEnabled())
            << "server parameter on, feature flag off";
    }
    {
        unittest::ServerParameterGuard flagOn{"featureFlagMaxKeyDetection", true};
        ASSERT_TRUE(sharding_util::isMaxKeyDetectionEnabled())
            << "feature flag on, server parameter off";
    }

    ASSERT_FALSE(sharding_util::isMaxKeyDetectionEnabled()) << "guards restored: both off again";
}

TEST_F(MaxKeyOrphanDetectionFixture, StepUpLauncherDoesNotRunDetectorWhenGateDisabled) {
    unittest::ServerParameterGuard flagOff{"featureFlagMaxKeyDetection", false};
    unittest::ServerParameterGuard paramOff{"enableMaxKeyDetection", false};
    ASSERT_FALSE(sharding_util::isMaxKeyDetectionEnabled());

    launchMaxKeyOrphanDetectionOnStepUp(opCtx, kStartingTerm);
    cancelMaxKeyOrphanDetection(opCtx->getServiceContext());

    ASSERT_FALSE(readMaxKeyOrphanScanStateDoc(opCtx).has_value())
        << "Detection must not run when both the feature flag and enableMaxKeyDetection are off";
}

TEST(MaxKeyOrphanDetectionTest, ShardingStatisticsReportIncludesOrphanScanFields) {
    ShardingStatistics stats;
    stats.maxKeyOrphanScanComplete.store(1);
    stats.maxKeyOrphanScanFoundUnownedMaxKey.store(1);
    stats.maxKeyOrphanScanUnownedAlertEmitted.store(1);
    stats.maxKeyOrphanScanErrors.store(3);
    stats.maxKeyOrphanScanFoundOwnedMaxKey.store(1);
    stats.maxKeyOrphanScanOwnedAlertEmitted.store(1);

    BSONObjBuilder bob;
    stats.report(&bob);
    const BSONObj obj = bob.obj();

    ASSERT_EQ(1LL, obj["maxKeyOrphanScanComplete"].Long());
    ASSERT_EQ(1LL, obj["maxKeyOrphanScanFoundUnownedMaxKey"].Long());
    ASSERT_EQ(1LL, obj["maxKeyOrphanScanUnownedAlertEmitted"].Long());
    ASSERT_EQ(3LL, obj["maxKeyOrphanScanErrors"].Long());
    ASSERT_EQ(1LL, obj["maxKeyOrphanScanFoundOwnedMaxKey"].Long());
    ASSERT_EQ(1LL, obj["maxKeyOrphanScanOwnedAlertEmitted"].Long());
}

TEST(MaxKeyOrphanDetectionTest, ShardingStatisticsOrphanScanFieldsDefaultToZero) {
    ShardingStatistics stats;

    BSONObjBuilder bob;
    stats.report(&bob);
    const BSONObj obj = bob.obj();

    ASSERT_EQ(0LL, obj["maxKeyOrphanScanComplete"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanFoundUnownedMaxKey"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanUnownedAlertEmitted"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanErrors"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanFoundOwnedMaxKey"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanOwnedAlertEmitted"].Long());
}

// Creates a collection with the given index and documents and returns its UUID. createIndexes
// implicitly creates the collection.
UUID createIndexedCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BSONObj& indexKey,
                             const std::vector<BSONObj>& docs) {
    DBDirectClient client(opCtx);
    BSONObj res;
    ASSERT(client.runCommand(nss.dbName(),
                             BSON("createIndexes" << nss.coll() << "indexes"
                                                  << BSON_ARRAY(BSON("key" << indexKey << "name"
                                                                           << "shardKeyIdx"))),
                             res))
        << res;
    for (const auto& doc : docs) {
        client.insert(nss, doc);
    }
    auto infos = client.getCollectionInfos(nss.dbName(), BSON("name" << nss.coll()));
    ASSERT_FALSE(infos.empty());
    return uassertStatusOK(UUID::parse((*infos.begin())["info"]["uuid"]));
}

const ChunkRange kGlobalMaxRange{BSON("a" << MINKEY), BSON("a" << MAXKEY)};
const ChunkRange kCompoundGlobalMaxRange{BSON("a" << MINKEY << "b" << MINKEY),
                                         BSON("a" << MAXKEY << "b" << MAXKEY)};

TEST_F(MaxKeyOrphanDetectionFixture, GuardSkipsWhenMaxKeyDocInGlobalMaxRange) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.coll1");
    auto uuid =
        createIndexedCollection(opCtx, nss, BSON("a" << 1), {BSON("a" << 5), BSON("a" << MAXKEY)});
    ASSERT_TRUE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1), kGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardDoesNotSkipWhenNoMaxKeyDoc) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.coll2");
    auto uuid =
        createIndexedCollection(opCtx, nss, BSON("a" << 1), {BSON("a" << 5), BSON("a" << 9)});
    ASSERT_FALSE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1), kGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardDoesNotSkipNonGlobalMaxRange) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.coll3");
    auto uuid = createIndexedCollection(opCtx, nss, BSON("a" << 1), {BSON("a" << MAXKEY)});
    const ChunkRange range{BSON("a" << MINKEY), BSON("a" << 100)};
    ASSERT_FALSE(
        shouldSkipRangeDeletionForMaxKeyOrphans(opCtx, nss.dbName(), uuid, BSON("a" << 1), range));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardDoesNotSkipHashedShardKey) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.coll4");
    auto uuid = createIndexedCollection(opCtx, nss, BSON("a" << "hashed"), {BSON("a" << 5)});
    ASSERT_FALSE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << "hashed"), kGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardSkipsWithWiderIndex) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.coll5");
    auto uuid =
        createIndexedCollection(opCtx,
                                nss,
                                BSON("a" << 1 << "b" << 1),
                                {BSON("a" << 5 << "b" << 1), BSON("a" << MAXKEY << "b" << 10)});
    ASSERT_TRUE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1), kGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardSkipsCompoundGlobalMaxDoc) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.compoundMax");
    auto uuid =
        createIndexedCollection(opCtx,
                                nss,
                                BSON("a" << 1 << "b" << 1),
                                {BSON("a" << 5 << "b" << 5), BSON("a" << MAXKEY << "b" << MAXKEY)});
    ASSERT_TRUE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1 << "b" << 1), kCompoundGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardDoesNotSkipCompoundPartialLeadingMaxKey) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.compoundPartial");
    auto uuid = createIndexedCollection(
        opCtx,
        nss,
        BSON("a" << 1 << "b" << 1),
        {BSON("a" << MAXKEY << "b" << 5), BSON("a" << MAXKEY << "b" << MINKEY)});
    ASSERT_FALSE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1 << "b" << 1), kCompoundGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardSkipsCompoundGlobalMaxThroughWiderIndex) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.compoundWider");
    auto uuid = createIndexedCollection(opCtx,
                                        nss,
                                        BSON("a" << 1 << "b" << 1 << "c" << 1),
                                        {BSON("a" << 5 << "b" << 5 << "c" << 5),
                                         BSON("a" << MAXKEY << "b" << MAXKEY << "c" << 10)});
    ASSERT_TRUE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1 << "b" << 1), kCompoundGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardDoesNotSkipCompoundPartialThroughWiderIndex) {
    const auto nss =
        NamespaceString::createNamespaceString_forTest("maxKeyGuard.compoundWiderPartial");
    auto uuid = createIndexedCollection(opCtx,
                                        nss,
                                        BSON("a" << 1 << "b" << 1 << "c" << 1),
                                        {BSON("a" << MAXKEY << "b" << 1 << "c" << 10)});
    ASSERT_FALSE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1 << "b" << 1), kCompoundGlobalMaxRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardSkipsRefineDemotedOrphan) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.refineDemoted");
    auto uuid = createIndexedCollection(
        opCtx, nss, BSON("a" << 1 << "b" << 1), {BSON("a" << MAXKEY << "b" << 5)});
    // Pre-refine task range: upper bound is the single-field global max {a: MaxKey}.
    const ChunkRange preRefineRange{BSON("a" << MINKEY), BSON("a" << MAXKEY)};
    ASSERT_TRUE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1 << "b" << 1), preRefineRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardSkipsRefineDemotedCompoundOrphan) {
    const auto nss =
        NamespaceString::createNamespaceString_forTest("maxKeyGuard.refineDemotedCompound");
    auto uuid = createIndexedCollection(opCtx,
                                        nss,
                                        BSON("a" << 1 << "b" << 1 << "c" << 1),
                                        {BSON("a" << MAXKEY << "b" << MAXKEY << "c" << 5)});
    const ChunkRange preRefineRange{BSON("a" << MINKEY << "b" << MINKEY),
                                    BSON("a" << MAXKEY << "b" << MAXKEY)};
    ASSERT_TRUE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1 << "b" << 1 << "c" << 1), preRefineRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardSkipsRefineDemotedOrphanThroughWiderIndex) {
    const auto nss =
        NamespaceString::createNamespaceString_forTest("maxKeyGuard.refineDemotedWiderIndex");
    auto uuid = createIndexedCollection(opCtx,
                                        nss,
                                        BSON("a" << 1 << "b" << 1 << "c" << 1),
                                        {BSON("a" << MAXKEY << "b" << 5 << "c" << 10)});
    const ChunkRange preRefineRange{BSON("a" << MINKEY), BSON("a" << MAXKEY)};
    ASSERT_TRUE(shouldSkipRangeDeletionForMaxKeyOrphans(
        opCtx, nss.dbName(), uuid, BSON("a" << 1 << "b" << 1), preRefineRange));
}

TEST_F(MaxKeyOrphanDetectionFixture, GuardThrowsWhenShardKeyIndexMissing) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.coll6");
    DBDirectClient client(opCtx);
    client.insert(nss, BSON("a" << MAXKEY));  // Only the default _id index exists.
    auto infos = client.getCollectionInfos(nss.dbName(), BSON("name" << nss.coll()));
    ASSERT_FALSE(infos.empty());
    auto uuid = uassertStatusOK(UUID::parse((*infos.begin())["info"]["uuid"]));
    ASSERT_THROWS_CODE(shouldSkipRangeDeletionForMaxKeyOrphans(
                           opCtx, nss.dbName(), uuid, BSON("a" << 1), kGlobalMaxRange),
                       DBException,
                       ErrorCodes::IndexNotFound);
}

// Inserts a range-deletion task into config.rangeDeletions and returns its id.
UUID insertRangeDeletionTask(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const UUID& collUuid,
                             const ChunkRange& range,
                             boost::optional<BSONObj> keyPattern) {
    RangeDeletionTask task;
    const auto taskId = UUID::gen();
    task.setId(taskId);
    task.setNss(nss);
    task.setCollectionUuid(collUuid);
    task.setDonorShardId(ShardId("donorShard"));
    task.setRange(range);
    task.setWhenToClean(CleanWhenEnum::kNow);
    if (keyPattern) {
        task.setKeyPattern(KeyPattern(*keyPattern));
    }
    DBDirectClient client(opCtx);
    client.insert(NamespaceString::kRangeDeletionNamespace, task.toBSON());
    return taskId;
}

// Returns the blockedTasks ids recorded in the state doc, or boost::none if the field is absent
// (field absence, not emptiness, means the classification has not yet run for this epoch).
boost::optional<std::vector<UUID>> readBlockedTasks(OperationContext* opCtx) {
    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    if (!doc || !doc->hasField("blockedTasks")) {
        return boost::none;
    }
    std::vector<UUID> ids;
    for (auto&& elem : doc->getField("blockedTasks").Array()) {
        ids.push_back(uassertStatusOK(UUID::parse(elem)));
    }
    return ids;
}

TEST_F(MaxKeyOrphanDetectionFixture, ClassifyBlocksGlobalMaxTaskWithMaxKeyDoc) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.blocked");
    auto uuid =
        createIndexedCollection(opCtx, nss, BSON("a" << 1), {BSON("a" << 5), BSON("a" << MAXKEY)});
    auto taskId = insertRangeDeletionTask(opCtx, nss, uuid, kGlobalMaxRange, BSON("a" << 1));

    auto blocked = loadOrComputeBlockedMaxKeyRangeDeletionTasks(opCtx);
    ASSERT_EQ(1u, blocked.size());
    ASSERT_EQ(taskId, blocked[0]);

    auto persisted = readBlockedTasks(opCtx);
    ASSERT(persisted.has_value());
    ASSERT_EQ(1u, persisted->size());
    ASSERT_EQ(taskId, (*persisted)[0]);
}

TEST_F(MaxKeyOrphanDetectionFixture, ClassifyLeavesCleanTaskDeletable) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.clean");
    auto uuid = createIndexedCollection(opCtx, nss, BSON("a" << 1), {BSON("a" << 5)});
    insertRangeDeletionTask(opCtx, nss, uuid, kGlobalMaxRange, BSON("a" << 1));

    auto blocked = loadOrComputeBlockedMaxKeyRangeDeletionTasks(opCtx);
    ASSERT_TRUE(blocked.empty());

    // Present-but-empty blockedTasks marks the classification complete for this epoch.
    auto persisted = readBlockedTasks(opCtx);
    ASSERT(persisted.has_value());
    ASSERT_TRUE(persisted->empty());
    // The guard writes only blockedTasks; it must not affect detection-scan fields.
    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT_FALSE(doc->hasField("scanStartedAt")) << *doc;
    ASSERT_FALSE(doc->hasField("scanCompletedAt")) << *doc;
    ASSERT_FALSE(doc->hasField("foundUnownedMaxKey")) << *doc;
    ASSERT_FALSE(doc->hasField("unownedAlertEmitted")) << *doc;
}

TEST_F(MaxKeyOrphanDetectionFixture, ClassifyRehydratesFromPresentBlockedTasks) {
    // A task that would classify as clean, plus a state doc whose blockedTasks already lists an id.
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.rehydrate");
    auto uuid = createIndexedCollection(opCtx, nss, BSON("a" << 1), {BSON("a" << 5)});
    insertRangeDeletionTask(opCtx, nss, uuid, kGlobalMaxRange, BSON("a" << 1));

    const auto seededId = UUID::gen();
    DBDirectClient client(opCtx);
    client.remove(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace,
                  BSON("_id" << kScanStateIdValue));
    BSONArrayBuilder arr;
    seededId.appendToArrayBuilder(&arr);
    client.insert(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace,
                  BSON("_id" << kScanStateIdValue << "scanStartedAt" << Date_t::now()
                             << "foundUnownedMaxKey" << true << "unownedAlertEmitted" << true
                             << "blockedTasks" << arr.arr()));
    auto before = readMaxKeyOrphanScanStateDoc(opCtx);

    auto blocked = loadOrComputeBlockedMaxKeyRangeDeletionTasks(opCtx);

    // Rehydrated verbatim: the present set is returned and the classifying scan does not run.
    ASSERT_EQ(1u, blocked.size());
    ASSERT_EQ(seededId, blocked[0]);
    auto after = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT_BSONOBJ_EQ(*before, *after);
}

TEST_F(MaxKeyOrphanDetectionFixture, ClassifyLeavesNonGlobalMaxTaskDeletable) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.nonGlobalMax");
    auto uuid =
        createIndexedCollection(opCtx, nss, BSON("a" << 1), {BSON("a" << 5), BSON("a" << MAXKEY)});
    const ChunkRange nonGlobalMax{BSON("a" << MINKEY), BSON("a" << 100)};
    insertRangeDeletionTask(opCtx, nss, uuid, nonGlobalMax, BSON("a" << 1));

    auto blocked = loadOrComputeBlockedMaxKeyRangeDeletionTasks(opCtx);
    ASSERT_TRUE(blocked.empty());
}

TEST_F(MaxKeyOrphanDetectionFixture, ClassifyLeavesDroppedCollectionTaskDeletable) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.dropped");
    insertRangeDeletionTask(opCtx, nss, UUID::gen(), kGlobalMaxRange, BSON("a" << 1));

    auto blocked = loadOrComputeBlockedMaxKeyRangeDeletionTasks(opCtx);
    ASSERT_TRUE(blocked.empty());
}

TEST_F(MaxKeyOrphanDetectionFixture, ClassifyConservativelyBlocksUnclassifiableTask) {
    const auto nss = NamespaceString::createNamespaceString_forTest("maxKeyGuard.unclassifiable");
    DBDirectClient client(opCtx);
    client.insert(nss, BSON("a" << MAXKEY));  // Only the default _id index exists.
    auto infos = client.getCollectionInfos(nss.dbName(), BSON("name" << nss.coll()));
    ASSERT_FALSE(infos.empty());
    auto uuid = uassertStatusOK(UUID::parse((*infos.begin())["info"]["uuid"]));
    auto taskId = insertRangeDeletionTask(opCtx, nss, uuid, kGlobalMaxRange, BSON("a" << 1));

    auto blocked = loadOrComputeBlockedMaxKeyRangeDeletionTasks(opCtx);
    ASSERT_EQ(1u, blocked.size());
    ASSERT_EQ(taskId, blocked[0]);
}

TEST_F(MaxKeyOrphanDetectionFixture, ClassifyThrowsOnUnreadableStateDoc) {
    DBDirectClient client(opCtx);
    client.insert(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace,
                  BSON("_id" << kScanStateIdValue << "blockedTasks" << 42));
    ASSERT_THROWS(loadOrComputeBlockedMaxKeyRangeDeletionTasks(opCtx), DBException);
}

}  // namespace
}  // namespace mongo
