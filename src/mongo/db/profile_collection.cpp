/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/profile_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/query/util/throughput_gauge.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrent_shared_values_map.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <queue>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::profile_collection {

namespace {

MONGO_FAIL_POINT_DEFINE(forceLockTimeoutForProfiler);

AtomicWord<int64_t> profilerWritesTotal{0};
AtomicWord<int64_t> profilerWritesActive{0};

// Under heavy load we will choose to abandon and drop profile writes to preserve availability.
// The observability tool shouldn't cause an availability problem. This metric serves to capture
// when this is happening. This mechanism operates on a db scope. One db could be abandoning
// writes to the point where the profiler is entirely disabled, and another could be operating
// smoothly.
struct AbandonedWriteMetrics {
    ThroughputGauge throughputGauge;
    AtomicWord<Date_t> tsDisabled;
};

ConcurrentSharedValuesMap<DatabaseName, AbandonedWriteMetrics> profilerAbandonmentMetrics;

// Track some overall counters to report in serverStatus. Reporting a map by dbName is potentially
// too large for serverStatus.
AtomicWord<int64_t> profilerWritesAbandondedGlobally{0};

// Please note that this counter will not ever reset/decrease, but writes to the profiler can be
// re-activated by raising the cap. If the cap is raised and then hit again, this counter will
// double-increment for the same db.
AtomicWord<int64_t> dbsPastThreshold{0};

static const auto profilerDisabledWarningString =
    "The profiler in this db has been automatically disabled due to server load. This tool is "
    "known to have a high overhead and can cause performance problems if turned up too high. On a "
    "per-db basis, the system will watch for lock acquisition timeouts while attempting to acquire "
    "a lock for profiling purposes. The threshold for this timeout is controlled by the server "
    "parameter 'internalQueryGlobalProfilingLockDeadlineMs.' If there are more timeouts than the "
    "configured threshold given by 'internalProfilingMaxAbandonedWritesPerSecondPerDb', then all "
    "future profile writes are disabled by setting the profile level to 0 for this db. It is "
    "recommended that future attempts to profile use a lower sample rate to avoid an outsized "
    "impact.";

class ProfilerSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~ProfilerSection() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder bob;
        bob.append("totalWrites", profilerWritesTotal.loadRelaxed());
        bob.append("activeWriters", profilerWritesActive.loadRelaxed());
        bob.append("totalAbandonedWrites", profilerWritesAbandondedGlobally.loadRelaxed());
        bob.append("dbsPastThreshold", dbsPastThreshold.loadRelaxed());
        return bob.obj();
    }
};

auto& profilerSection = *ServerStatusSectionBuilder<ProfilerSection>("profiler").forShard();

auto buildProfileObject(auto opCtx) {
    // Initialize with 1kb at start in order to avoid realloc later
    BufBuilder profileBufBuilder(1024);

    BSONObjBuilder profileObjBuilder(profileBufBuilder);

    {
        auto curOp = CurOp::get(opCtx);

        Locker::LockerInfo lockerInfo;
        shard_role_details::getLocker(opCtx)->getLockerInfo(&lockerInfo, curOp->getLockStatsBase());

        auto storageMetrics = curOp->getOperationStorageMetrics();

        curOp->debug().append(opCtx,
                              lockerInfo.stats,
                              shard_role_details::getLocker(opCtx)->getFlowControlStats(),
                              storageMetrics,
                              curOp->getPrepareReadConflicts(),
                              false /*omitCommand*/,
                              profileObjBuilder);
    }

    profileObjBuilder.appendDate("ts", Date_t::now());
    profileObjBuilder.append("client", opCtx->getClient()->clientAddress());

    if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
        auto appName = clientMetadata->getApplicationName();
        if (!appName.empty()) {
            profileObjBuilder.append("appName", appName);
        }
    }

    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
    OpDebug::appendUserInfo(*CurOp::get(opCtx), profileObjBuilder, authSession);
    return profileObjBuilder.done().redact(BSONObj::RedactLevel::sensitiveOnly);
}

BSONObj encodeProfileSettings(const ProfileSettings& dbProfileSettings) {
    BSONObjBuilder settingsBuilder;
    settingsBuilder.append("level", dbProfileSettings.level);
    if (dbProfileSettings.filter) {
        settingsBuilder.append("filter", dbProfileSettings.filter->serialize());
    } else {
        settingsBuilder.append("filter", "unset"_sd);
    }

    return settingsBuilder.obj();
}

// Type tag to indicate at the call site that we want to opt out of a lock deadline.
struct NoTimeoutTag {};

void doProfile(auto opCtx,
               const auto& nss,
               const BSONObj& profileObj,
               std::variant<Milliseconds, NoTimeoutTag> lockTimeout) {
    // We create a new opCtx so that we aren't interrupted by having the original operation
    // killed or timed out. Those are the case we want to have profiling data.
    auto newClient =
        opCtx->getServiceContext()->getService(ClusterRole::ShardServer)->makeClient("profiling");
    auto newCtx = newClient->makeOperationContext();

    // We swap the lockers as that way we preserve locks held in transactions and any other
    // options set for the locker like maxLockTimeout.
    auto oldLocker =
        shard_role_details::swapLocker(opCtx, std::make_unique<Locker>(opCtx->getServiceContext()));
    auto emptyLocker = shard_role_details::swapLocker(newCtx.get(), std::move(oldLocker));
    ON_BLOCK_EXIT([&] {
        auto oldCtxLocker = shard_role_details::swapLocker(newCtx.get(), std::move(emptyLocker));
        shard_role_details::swapLocker(opCtx, std::move(oldCtxLocker));
    });
    AlternativeClientRegion acr(newClient);
    const auto dbProfilingNS = NamespaceString::makeSystemDotProfileNamespace(nss.dbName());

    profilerWritesActive.fetchAndAddRelaxed(1);
    ON_BLOCK_EXIT([&] { profilerWritesActive.fetchAndSubtractRelaxed(1); });

    boost::optional<CollectionAcquisition> profileCollection;
    while (true) {
        const auto deadline =
            std::visit(OverloadedVisitor{[&](const NoTimeoutTag&) { return Date_t::max(); },
                                         [&](const Milliseconds& millis) {
                                             return Date_t::now() + millis;
                                         }},
                       lockTimeout);

        profileCollection.emplace(acquireCollection(
            newCtx.get(),
            CollectionAcquisitionRequest(dbProfilingNS,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         repl::ReadConcernArgs::get(newCtx.get()),
                                         AcquisitionPrerequisites::kUnreplicatedWrite,
                                         deadline),
            MODE_IX));
        if (MONGO_unlikely(forceLockTimeoutForProfiler.shouldFail()) &&
            !std::holds_alternative<NoTimeoutTag>(lockTimeout)) {
            uasserted(ErrorCodes::LockTimeout,
                      str::stream() << "forcing LockTimeout based on 'forceLockTimeoutForProfiler' "
                                       "fail point. profileObj="
                                    << profileObj);
        }

        Database* const db =
            DatabaseHolder::get(newCtx.get())->getDb(newCtx.get(), dbProfilingNS.dbName());
        if (!db) {
            // Database disappeared.
            LOGV2(20700, "note: not profiling because db went away for namespace", logAttrs(nss));
            return;
        }

        if (profileCollection->exists()) {
            break;
        }

        uassertStatusOK(createProfileCollection(newCtx.get(), db));
        profileCollection.reset();
    }

    invariant(profileCollection && profileCollection->exists());

    WriteUnitOfWork wuow(newCtx.get());
    OpDebug* const nullOpDebug = nullptr;
    uassertStatusOK(collection_internal::insertDocument(newCtx.get(),
                                                        profileCollection->getCollectionPtr(),
                                                        InsertStatement(profileObj),
                                                        nullOpDebug,
                                                        false));
    wuow.commit();
    profilerWritesTotal.fetchAndAddRelaxed(1);
}

/**
 * Returns true if this abandoned write should be logged. This abandonment can happen a lot under
 * load. Let's log when this happens, but not every time.
 */
bool noteThereWasAnAbandonedWrite(auto opCtx, const auto& abandonmentMetrics) {
    abandonmentMetrics->throughputGauge.recordEvent(Date_t::now());
    profilerWritesAbandondedGlobally.fetchAndAddRelaxed(1);
    static Rarely sampler;
    if (sampler.tick()) {
        // Every once and a while (Rarely's frequency), log the event.
        return true;
    }
    return false;
}

BSONObj metricsToBson(auto nAbandonedInLastSecond, Date_t tsDisabled) {
    BSONObjBuilder metricsObjBuilder;
    metricsObjBuilder.append("nAbandonedInLastSecond", nAbandonedInLastSecond);
    if (tsDisabled != Date_t::min()) {
        metricsObjBuilder.append("fullyDisabledAt", tsDisabled);
    }
    return metricsObjBuilder.obj();
}

void disableProblematicProfiling(auto opCtx,
                                 const auto& nss,
                                 const Date_t tsDisabled,
                                 const auto& abandonmentMetrics,
                                 const auto& nAbandonedInLastSecond) {
    // Set profiling level to 0 to prevent future writes.
    auto& dbProfileSettings = DatabaseProfileSettings::get(opCtx->getServiceContext());
    ProfileSettings oldSettings{dbProfileSettings.getDatabaseProfileSettings(nss.dbName())};
    ProfileSettings newSettings{oldSettings};
    newSettings.level = 0;
    dbProfileSettings.setDatabaseProfileSettings(nss.dbName(), newSettings);

    const auto maxAbandonedWrites = internalProfilingMaxAbandonedWritesPerSecondPerDb.loadRelaxed();

    const auto metricsBson = metricsToBson(nAbandonedInLastSecond, tsDisabled);

    LOGV2_WARNING(11119100,
                  "Abandoned too many profile writes. In a further attempt to maintain "
                  "performance, profiling is disabled for this db until settings are manually "
                  "updated. Profile settings changed.",
                  "db"_attr = nss.dbName(),
                  "oldProfileSettings"_attr = encodeProfileSettings(oldSettings),
                  "newProfileSettings"_attr = encodeProfileSettings(newSettings),
                  "abandonmentMetrics"_attr = metricsBson,
                  "maxAbandonedWritesPerSecond"_attr = maxAbandonedWrites);
    dbsPastThreshold.fetchAndAdd(1);

    auto noteToStoreInProfile =
        BSON("ts" << Date_t::now() << "note" << profilerDisabledWarningString
                  << "internalQueryGlobalProfilingLockDeadlineMs"
                  << internalQueryGlobalProfilingLockDeadlineMs.loadRelaxed()
                  << "internalProfilingMaxAbandonedWritesPerSecondPerDb" << maxAbandonedWrites
                  << "slowms" << serverGlobalParams.slowMS.loadRelaxed() << "abandonmentMetrics"
                  << metricsBson << "profileSettings"
                  << BSON("was" << encodeProfileSettings(oldSettings) << "new"
                                << encodeProfileSettings(newSettings)));
    try {
        doProfile(opCtx, nss, noteToStoreInProfile, NoTimeoutTag{});
    } catch (const AssertionException& assertionEx) {
        LOGV2_WARNING(11119104,
                      "Caught Assertion while trying to write down decision to disable profiler",
                      logAttrs(nss),
                      "assertion"_attr = redact(assertionEx),
                      "code"_attr = assertionEx.code());
    }
}

bool profilingHasBecomeProblematic(OperationContext* opCtx,
                                   Date_t now,
                                   const auto& nAbandonedInLastSecond) {
    return nAbandonedInLastSecond > internalProfilingMaxAbandonedWritesPerSecondPerDb.loadRelaxed();
}
}  // namespace

void profile(OperationContext* opCtx, NetworkOp op) {
    const auto nss = CurOp::get(opCtx)->getNSS();
    auto abandonmentMetrics = profilerAbandonmentMetrics.getOrEmplace(nss.dbName());

    try {
        doProfile(opCtx,
                  nss,
                  buildProfileObject(opCtx),
                  Milliseconds(internalQueryGlobalProfilingLockDeadlineMs.loadRelaxed()));
    } catch (const AssertionException& assertionEx) {
        bool shouldLog = true;
        if (assertionEx.code() == ErrorCodes::LockTimeout) {
            shouldLog = noteThereWasAnAbandonedWrite(opCtx, abandonmentMetrics);
        }
        if (shouldLog) {
            LOGV2_WARNING(20703,
                          "Caught Assertion while trying to profile operation",
                          "operation"_attr = networkOpToString(op),
                          logAttrs(nss),
                          "assertion"_attr = redact(assertionEx),
                          "code"_attr = assertionEx.code());
        }
    }

    auto now = opCtx->fastClockSource().now();
    const auto nAbandonedInLastSecond =
        abandonmentMetrics->throughputGauge.nEventsInPreviousSecond(now);
    if (profilingHasBecomeProblematic(opCtx, now, nAbandonedInLastSecond)) {

        disableProblematicProfiling(opCtx, nss, now, abandonmentMetrics, nAbandonedInLastSecond);
    }
}

Status createProfileCollection(OperationContext* opCtx, Database* db) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(db->name(), MODE_IX));

    const auto dbProfilingNS = NamespaceString::makeSystemDotProfileNamespace(db->name());

    if (!dbProfilingNS.isValid(DatabaseName::DollarInDbNameBehavior::Disallow)) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream()
                          << "Invalid database name: " << db->name().toStringForErrorMsg());
    }

    // Checking the collection exists must also be done in the WCE retry loop. Only retrying
    // collection creation would endlessly throw errors because the collection exists: must check
    // and see the collection exists in order to break free.
    return writeConflictRetry(opCtx, "createProfileCollection", dbProfilingNS, [&] {
        const Collection* collection =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, dbProfilingNS);
        if (collection) {
            if (!collection->isCapped()) {
                return Status(ErrorCodes::NamespaceExists,
                              str::stream() << dbProfilingNS.toStringForErrorMsg()
                                            << " exists but isn't capped");
            }

            return Status::OK();
        }

        // system.profile namespace doesn't exist; create it
        LOGV2(20701, "Creating profile collection", logAttrs(dbProfilingNS));

        CollectionOptions collectionOptions;
        collectionOptions.capped = true;
        collectionOptions.cappedSize = 1024ll * 1024;

        WriteUnitOfWork wunit(opCtx);
        repl::UnreplicatedWritesBlock uwb(opCtx);
        invariant(db->createCollection(opCtx, dbProfilingNS, collectionOptions));
        wunit.commit();

        return Status::OK();
    });
}

}  // namespace mongo::profile_collection
