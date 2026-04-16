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

#include "mongo/db/s/topology_change_helpers.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace topology_change_helpers {

boost::optional<RangeDeletionTask> getLatestNonProcessingRangeDeletionTask(
    OperationContext* opCtx) {
    AutoGetCollection collRangeDeletionLock(
        opCtx, NamespaceString::kRangeDeletionNamespace, MODE_S);
    DBDirectClient client(opCtx);

    // Get latest non processing range deletion task scheduled for future cleanup
    // We include pending tasks to avoid a race condition where moveChunk commits
    // before marking the range deletion task as non-pending (SERVER-119117).
    FindCommandRequest findCommand(NamespaceString::kRangeDeletionNamespace);
    findCommand.setFilter(BSON(RangeDeletionTask::kProcessingFieldName << BSON("$ne" << true)));
    findCommand.setSort(BSON(RangeDeletionTask::kTimestampFieldName << -1));
    auto bsonDoc = client.findOne(std::move(findCommand));
    if (bsonDoc.isEmpty()) {
        return boost::none;
    }
    return RangeDeletionTask::parse(
        IDLParserContext("getLatestNonPendingNonProcessingRangeDeletionTask"), bsonDoc);
}

bool checkOrphanCleanupDelayElapsed(OperationContext* opCtx, const RangeDeletionTask& task) {
    auto elapsedSec =
        getGlobalServiceContext()->getFastClockSource()->now().toMillisSinceEpoch() / 1000 -
        task.getTimestamp()->getSecs();
    // Note that in the normal range deletions workflow we begin waiting for
    // orphanCleanupDelaySecs after pending field is unset and it is marked as processing by the
    // range deleter service. Here the behavior is different and we wait since the time the task
    // was registered in DB.
    if (elapsedSec < orphanCleanupDelaySecs.load()) {
        LOGV2(1039900,
              "removeShard: waiting for orphanCleanupDelaySecs to complete",
              "elapsed"_attr = elapsedSec,
              "orphanCleanupDelaySecs"_attr = orphanCleanupDelaySecs.load());
        return false;
    }
    return true;
}

}  // namespace topology_change_helpers

}  // namespace mongo
