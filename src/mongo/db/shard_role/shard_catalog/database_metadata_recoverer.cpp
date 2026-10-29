// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_metadata_recoverer.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_util.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(avoidTassertForInconsistentMetadata);

namespace shard_catalog_recoverer {

AttemptResult onDbVersionMismatchAuthoritative(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const DatabaseVersion& receivedDbVersion) {
    // If this node is a secondary, and the version received from the router may be older than the
    // cached version, there is no point in proceeding unless the oplog has been applied up to the
    // timestamp referenced by the received version.
    // On the other hand, if this node is a primary, it should have already applied the timestamp
    // received from the router, making this a no-op.
    // Additionally, we need to wait to see the timestamp with majority read concern to avoid split-
    // brain scenarios, where writes with local read concern might bypass the following wait, and we
    // end up seeing an intermediate state.
    const auto targetOpTime =
        repl::OpTime(receivedDbVersion.getTimestamp(), repl::OpTime::kUninitializedTerm);

    Timer dbVersionWaitTimer;
    repl::ReplicationCoordinator::get(opCtx)
        ->registerWaiterForMajorityReadOpTime(opCtx, targetOpTime)
        .get(opCtx);
    ShardingStatistics::get(opCtx).databaseShardingMetadataStatistics.registerDbVersionMismatchWait(
        dbVersionWaitTimer.millis());

    auto scopedDsr = boost::make_optional(DatabaseShardingRuntime::acquireShared(opCtx, dbName));

    if (refresh_util::waitForCriticalSectionIfNeeded(opCtx, scopedDsr)) {
        // Waited for another thread to exit from the critical section, so retry.
        return AttemptResult::kRetry;
    }

    // From now until the end of this block: no thread is in the critical section or can enter
    // it (would require to exclusive lock the DSS). Therefore, the database version can be
    // accessed safely.

    const auto dbVersion = (*scopedDsr)->getDbVersion(opCtx);

    // If shards are the authoritative source for database metadata, at this stage this node
    // has waited until the received version's optime and that any necessary critical section
    // has been released. This guarantees the following:
    //
    //      1) If there is an entry in the DSS, it means the database information is up to date.
    //      In this case, we either serve the request (if both versions match) or inform the
    //      router that its version is stale.
    //
    //      2) If there is no entry in the DSS, it indicates that another DDL operation has
    //      moved the database elsewhere or dropped, meaning this node is no longer the primary
    //      shard for this database.

    uassert(StaleDbRoutingVersion(dbName, receivedDbVersion, boost::none),
            str::stream() << "No cached info for the database " << dbName.toStringForErrorMsg(),
            dbVersion);

    const auto wantedVersion = *dbVersion;

    if (MONGO_unlikely(avoidTassertForInconsistentMetadata.shouldFail())) {
        uassert(StaleDbRoutingVersion(dbName, receivedDbVersion, wantedVersion),
                str::stream() << "Version mismatch for the database: "
                              << dbName.toStringForErrorMsg()
                              << ". Shard is authoritative and we have waited long enough for it "
                                 "to catch up. It can't have a version behind the routers anymore.",
                receivedDbVersion <= wantedVersion);
    } else {
        tassert(StaleDbRoutingVersion(dbName, receivedDbVersion, wantedVersion),
                str::stream() << "Version mismatch for the database: "
                              << dbName.toStringForErrorMsg()
                              << ". Shard is authoritative and we have waited long enough for it "
                                 "to catch up. It can't have a version behind the routers anymore.",
                receivedDbVersion <= wantedVersion);
    }

    uassert(StaleDbRoutingVersion(dbName, receivedDbVersion, wantedVersion),
            str::stream() << "Version mismatch for the database " << dbName.toStringForErrorMsg(),
            receivedDbVersion == wantedVersion);

    LOGV2_DEBUG(12920514,
                2,
                "Authoritative database version mismatch handled",
                "db"_attr = dbName,
                "receivedDbVersion"_attr = receivedDbVersion,
                "waitMillis"_attr = dbVersionWaitTimer.millis());

    return AttemptResult::kDone;
}

}  // namespace shard_catalog_recoverer
}  // namespace mongo
