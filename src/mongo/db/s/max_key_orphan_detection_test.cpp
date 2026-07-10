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

#include "mongo/db/s/max_key_orphan_detection.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
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
                                  bool foundMaxKey,
                                  bool alertEmitted) {
    DBDirectClient client(opCtx);
    client.remove(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace,
                  BSON("_id" << kScanStateIdValue));
    BSONObjBuilder docBob;
    docBob.append("_id", kScanStateIdValue);
    docBob.append("scanStartedAt", Date_t::now());
    if (scanComplete) {
        docBob.append("scanCompletedAt", Date_t::now());
    }
    docBob.append("foundMaxKey", foundMaxKey);
    docBob.append("alertEmitted", alertEmitted);
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
    ASSERT_FALSE(doc->getField("foundMaxKey").Bool()) << *doc;
    ASSERT_FALSE(doc->getField("alertEmitted").Bool()) << *doc;
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
    ASSERT_FALSE(doc->getField("foundMaxKey").Bool());
}

TEST_F(MaxKeyOrphanDetectionFixture, ReRunsWhenPriorScanIncomplete) {
    seedMaxKeyOrphanScanStateDoc(
        opCtx, /*scanComplete=*/false, /*foundMaxKey=*/false, /*alertEmitted=*/false);

    runMaxKeyOrphanDetection(opCtx, kStartingTerm);

    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(doc.has_value());
    ASSERT(doc->hasField("scanCompletedAt")) << *doc;
}

TEST_F(MaxKeyOrphanDetectionFixture, PreservesAlertEmittedAcrossRescan) {
    seedMaxKeyOrphanScanStateDoc(
        opCtx, /*scanComplete=*/false, /*foundMaxKey=*/false, /*alertEmitted=*/true);

    runMaxKeyOrphanDetection(opCtx, kStartingTerm);

    auto doc = readMaxKeyOrphanScanStateDoc(opCtx);
    ASSERT(doc.has_value());
    ASSERT(doc->hasField("scanCompletedAt")) << *doc;
    ASSERT_TRUE(doc->getField("alertEmitted").Bool())
        << "Expected the re-scan to preserve a prior alertEmitted=true: " << *doc;
}

TEST(MaxKeyOrphanDetectionTest, ShardingStatisticsReportIncludesOrphanScanFields) {
    ShardingStatistics stats;
    stats.maxKeyOrphanScanComplete.store(1);
    stats.maxKeyOrphanScanFoundMaxKey.store(1);
    stats.maxKeyOrphanScanAlertEmitted.store(1);
    stats.maxKeyOrphanScanErrors.store(3);

    BSONObjBuilder bob;
    stats.report(&bob);
    const BSONObj obj = bob.obj();

    ASSERT_EQ(1LL, obj["maxKeyOrphanScanComplete"].Long());
    ASSERT_EQ(1LL, obj["maxKeyOrphanScanFoundMaxKey"].Long());
    ASSERT_EQ(1LL, obj["maxKeyOrphanScanAlertEmitted"].Long());
    ASSERT_EQ(3LL, obj["maxKeyOrphanScanErrors"].Long());
}

TEST(MaxKeyOrphanDetectionTest, ShardingStatisticsOrphanScanFieldsDefaultToZero) {
    ShardingStatistics stats;

    BSONObjBuilder bob;
    stats.report(&bob);
    const BSONObj obj = bob.obj();

    ASSERT_EQ(0LL, obj["maxKeyOrphanScanComplete"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanFoundMaxKey"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanAlertEmitted"].Long());
    ASSERT_EQ(0LL, obj["maxKeyOrphanScanErrors"].Long());
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
    ASSERT_FALSE(doc->hasField("foundMaxKey")) << *doc;
    ASSERT_FALSE(doc->hasField("alertEmitted")) << *doc;
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
                             << "foundMaxKey" << true << "alertEmitted" << true << "blockedTasks"
                             << arr.arr()));
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
