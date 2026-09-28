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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"


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
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << MAXKEY << "b" << 10)));
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << MAXKEY << "b" << MINKEY)));
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << 5)));
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << 5 << "b" << MAXKEY)));
    ASSERT_FALSE(isGlobalMaxShardKey(BSON("a" << MINKEY << "b" << MAXKEY)));
}

constexpr StringData kScanStateIdValue = "scanState"_sd;
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

class MaxKeyOrphanDetectionFixture : public ShardServerTestFixture {
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
    RAIIServerParameterControllerForTest _maxKeyDetectionFlag{"featureFlagMaxKeyDetection", true};
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
        RAIIServerParameterControllerForTest paramOn{"enableMaxKeyDetection", true};
        ASSERT_TRUE(sharding_util::isMaxKeyDetectionEnabled())
            << "server parameter on, feature flag off";
    }
    {
        RAIIServerParameterControllerForTest flagOn{"featureFlagMaxKeyDetection", true};
        ASSERT_TRUE(sharding_util::isMaxKeyDetectionEnabled())
            << "feature flag on, server parameter off";
    }

    ASSERT_FALSE(sharding_util::isMaxKeyDetectionEnabled()) << "guards restored: both off again";
}

TEST_F(MaxKeyOrphanDetectionFixture, StepUpLauncherDoesNotRunDetectorWhenGateDisabled) {
    RAIIServerParameterControllerForTest flagOff{"featureFlagMaxKeyDetection", false};
    RAIIServerParameterControllerForTest paramOff{"enableMaxKeyDetection", false};
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

}  // namespace
}  // namespace mongo
