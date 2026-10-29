// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/migration_util.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

using MigrationUtilsTest = ShardServerTestFixture;

UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    ASSERT(*autoColl);

    return autoColl->uuid();
}

template <typename ShardKey>
RangeDeletionTask createDeletionTask(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& uuid,
                                     ShardKey min,
                                     ShardKey max,
                                     ShardId donorShard = ShardId("donorShard"),
                                     bool pending = true) {
    auto task = RangeDeletionTask(UUID::gen(),
                                  nss,
                                  uuid,
                                  donorShard,
                                  ChunkRange{BSON("_id" << min), BSON("_id" << max)},
                                  CleanWhenEnum::kNow);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    task.setTimestamp(currentTime.clusterTime().asTimestamp());

    if (pending)
        task.setPending(true);

    return task;
}


TEST_F(MigrationUtilsTest, TestUpdateNumberOfOrphans) {
    auto opCtx = operationContext();
    const auto collectionUuid = UUID::gen();
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto rangeDeletionDoc = createDeletionTask(opCtx, kTestNss, collectionUuid, 0, 10);
    store.add(opCtx, rangeDeletionDoc);

    rangedeletionutil::persistUpdatedNumOrphans(
        opCtx, collectionUuid, rangeDeletionDoc.getRange(), 5);
    rangeDeletionDoc.setNumOrphanDocs(5);
    ASSERT_EQ(store.count(opCtx, rangeDeletionDoc.toBSON().removeField("timestamp")), 1);

    rangedeletionutil::persistUpdatedNumOrphans(
        opCtx, collectionUuid, rangeDeletionDoc.getRange(), -5);
    rangeDeletionDoc.setNumOrphanDocs(0);
    ASSERT_EQ(store.count(opCtx, rangeDeletionDoc.toBSON().removeField("timestamp")), 1);
}

}  // namespace
}  // namespace mongo
