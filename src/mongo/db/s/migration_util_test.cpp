/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/s/migration_util.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock.h"
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

    ASSERT(autoColl.getCollection());

    return autoColl.getCollection()->uuid();
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
