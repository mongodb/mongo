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


#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/health_log_interface.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/util/background.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {

repl::OpTime _logOp(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const boost::optional<UUID>& uuid,
                    const BSONObj& obj) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss);
    oplogEntry.setTid(nss.tenantId());
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(obj);
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
    return writeConflictRetry(
        opCtx, "dbCheck oplog entry", NamespaceString::kRsOplogNamespace.ns(), [&] {
            auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
            oplogEntry.setWallClockTime(clockSource->now());

            WriteUnitOfWork uow(opCtx);
            repl::OpTime result = repl::logOp(opCtx, &oplogEntry);
            uow.commit();
            return result;
        });
}

/**
 * RAII-style class, which logs dbCheck start and stop events in the healthlog and replicates them.
 */
class DbCheckStartAndStopLogger {
public:
    DbCheckStartAndStopLogger(OperationContext* opCtx) : _opCtx(opCtx) {
        try {
            const auto healthLogEntry = dbCheckHealthLogEntry(boost::none /*nss*/,
                                                              SeverityEnum::Info,
                                                              "",
                                                              OplogEntriesEnum::Start,
                                                              boost::none /*data*/
            );
            HealthLogInterface::get(_opCtx->getServiceContext())->log(*healthLogEntry);

            DbCheckOplogStartStop oplogEntry;
            const auto nss = NamespaceString::kAdminCommandNamespace;
            oplogEntry.setNss(nss);
            oplogEntry.setType(OplogEntriesEnum::Start);
            _logOp(_opCtx, nss, boost::none /*uuid*/, oplogEntry.toBSON());
        } catch (const DBException&) {
            LOGV2(6202200, "Could not log start event");
        }
    }

    ~DbCheckStartAndStopLogger() {
        try {
            DbCheckOplogStartStop oplogEntry;
            const auto nss = NamespaceString::kAdminCommandNamespace;
            oplogEntry.setNss(nss);
            oplogEntry.setType(OplogEntriesEnum::Stop);
            _logOp(_opCtx, nss, boost::none /*uuid*/, oplogEntry.toBSON());

            const auto healthLogEntry = dbCheckHealthLogEntry(boost::none /*nss*/,
                                                              SeverityEnum::Info,
                                                              "",
                                                              OplogEntriesEnum::Stop,
                                                              boost::none /*data*/
            );
            HealthLogInterface::get(_opCtx->getServiceContext())->log(*healthLogEntry);
        } catch (const DBException&) {
            LOGV2(6202201, "Could not log stop event");
        }
    }

private:
    OperationContext* _opCtx;
};

/**
 * All the information needed to run dbCheck on a single collection.
 */
struct DbCheckCollectionInfo {
    NamespaceString nss;
    UUID uuid;
    BSONKey start;
    BSONKey end;
    int64_t maxCount;
    int64_t maxSize;
    int64_t maxRate;
    int64_t maxDocsPerBatch;
    int64_t maxBytesPerBatch;
    int64_t maxBatchTimeMillis;
    WriteConcernOptions writeConcern;
};

/**
 * A run of dbCheck consists of a series of collections.
 */
using DbCheckRun = std::vector<DbCheckCollectionInfo>;

std::unique_ptr<DbCheckRun> singleCollectionRun(OperationContext* opCtx,
                                                const DatabaseName& dbName,
                                                const DbCheckSingleInvocation& invocation) {
    NamespaceString nss(
        NamespaceStringUtil::parseNamespaceFromRequest(dbName, invocation.getColl()));
    AutoGetCollectionForRead agc(opCtx, nss);

    uassert(ErrorCodes::NamespaceNotFound,
            "Collection " + invocation.getColl() + " not found",
            agc.getCollection());

    uassert(40619,
            "Cannot run dbCheck on " + nss.toString() + " because it is not replicated",
            nss.isReplicated());

    uassert(6769500, "dbCheck no longer supports snapshotRead:false", invocation.getSnapshotRead());

    const auto start = invocation.getMinKey();
    const auto end = invocation.getMaxKey();
    const auto maxCount = invocation.getMaxCount();
    const auto maxSize = invocation.getMaxSize();
    const auto maxRate = invocation.getMaxCountPerSecond();
    const auto maxDocsPerBatch = invocation.getMaxDocsPerBatch();
    const auto maxBytesPerBatch = invocation.getMaxBytesPerBatch();
    const auto maxBatchTimeMillis = invocation.getMaxBatchTimeMillis();
    const auto info = DbCheckCollectionInfo{nss,
                                            agc->uuid(),
                                            start,
                                            end,
                                            maxCount,
                                            maxSize,
                                            maxRate,
                                            maxDocsPerBatch,
                                            maxBytesPerBatch,
                                            maxBatchTimeMillis,
                                            invocation.getBatchWriteConcern()};
    auto result = std::make_unique<DbCheckRun>();
    result->push_back(info);
    return result;
}

std::unique_ptr<DbCheckRun> fullDatabaseRun(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const DbCheckAllInvocation& invocation) {
    uassert(ErrorCodes::InvalidNamespace,
            "Cannot run dbCheck on local database",
            dbName.db() != "local");

    AutoGetDb agd(opCtx, dbName, MODE_IS);
    uassert(ErrorCodes::NamespaceNotFound, "Database " + dbName.db() + " not found", agd.getDb());

    uassert(6769501, "dbCheck no longer supports snapshotRead:false", invocation.getSnapshotRead());

    const int64_t max = std::numeric_limits<int64_t>::max();
    const auto rate = invocation.getMaxCountPerSecond();
    const auto maxDocsPerBatch = invocation.getMaxDocsPerBatch();
    const auto maxBytesPerBatch = invocation.getMaxBytesPerBatch();
    const auto maxBatchTimeMillis = invocation.getMaxBatchTimeMillis();
    auto result = std::make_unique<DbCheckRun>();
    auto perCollectionWork = [&](const Collection* coll) {
        if (!coll->ns().isReplicated()) {
            return true;
        }
        DbCheckCollectionInfo info{coll->ns(),
                                   coll->uuid(),
                                   BSONKey::min(),
                                   BSONKey::max(),
                                   max,
                                   max,
                                   rate,
                                   maxDocsPerBatch,
                                   maxBytesPerBatch,
                                   maxBatchTimeMillis,
                                   invocation.getBatchWriteConcern()};
        result->push_back(info);
        return true;
    };
    mongo::catalog::forEachCollectionFromDb(opCtx, dbName, MODE_IS, perCollectionWork);

    return result;
}


/**
 * Factory function for producing DbCheckRun's from command objects.
 */
std::unique_ptr<DbCheckRun> getRun(OperationContext* opCtx,
                                   const DatabaseName& dbName,
                                   const BSONObj& obj) {
    BSONObjBuilder builder;

    // Get rid of generic command fields.
    for (const auto& elem : obj) {
        const auto& fieldName = elem.fieldNameStringData();
        if (!isGenericArgument(fieldName)) {
            builder.append(elem);
        }
    }

    BSONObj toParse = builder.obj();

    // If the dbCheck argument is a string, this is the per-collection form.
    if (toParse["dbCheck"].type() == BSONType::String) {
        return singleCollectionRun(
            opCtx,
            dbName,
            DbCheckSingleInvocation::parse(
                IDLParserContext("", false /*apiStrict*/, dbName.tenantId()), toParse));
    } else {
        // Otherwise, it's the database-wide form.
        return fullDatabaseRun(
            opCtx,
            dbName,
            DbCheckAllInvocation::parse(
                IDLParserContext("", false /*apiStrict*/, dbName.tenantId()), toParse));
    }
}

std::shared_ptr<const CollectionCatalog> getConsistentCatalogAndSnapshot(OperationContext* opCtx) {
    // Loop until we get a consistent catalog and snapshot
    while (true) {
        const auto catalogBeforeSnapshot = CollectionCatalog::get(opCtx);
        opCtx->recoveryUnit()->preallocateSnapshot();
        const auto catalogAfterSnapshot = CollectionCatalog::get(opCtx);
        if (catalogBeforeSnapshot == catalogAfterSnapshot) {
            return catalogBeforeSnapshot;
        }
        opCtx->recoveryUnit()->abandonSnapshot();
    }
}

/**
 * The BackgroundJob in which dbCheck actually executes on the primary.
 */
class DbCheckJob : public BackgroundJob {
public:
    DbCheckJob(const DatabaseName& dbName, std::unique_ptr<DbCheckRun> run)
        : BackgroundJob(true), _done(false), _dbName(dbName.toString()), _run(std::move(run)) {}

protected:
    virtual std::string name() const override {
        return "dbCheck";
    }

    virtual void run() override {
        // Every dbCheck runs in its own client.
        ThreadClient tc(name(), getGlobalServiceContext());

        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationKillableByStepdown(lk);
        }

        auto uniqueOpCtx = tc->makeOperationContext();
        auto opCtx = uniqueOpCtx.get();

        DbCheckStartAndStopLogger startStop(opCtx);

        for (const auto& coll : *_run) {
            try {
                _doCollection(opCtx, coll);
            } catch (const DBException& e) {
                auto logEntry = dbCheckErrorHealthLogEntry(
                    coll.nss, "dbCheck failed", OplogEntriesEnum::Batch, e.toStatus());
                HealthLogInterface::get(Client::getCurrent()->getServiceContext())->log(*logEntry);
                return;
            }

            if (_done) {
                LOGV2(20451, "dbCheck terminated due to stepdown");
                return;
            }
        }
    }

private:
    void _doCollection(OperationContext* opCtx, const DbCheckCollectionInfo& info) {
        if (_done) {
            return;
        }

        const std::string curOpMessage = "Scanning namespace " + info.nss.toString();
        ProgressMeterHolder progress;
        {
            AutoGetCollection coll(opCtx, info.nss, MODE_IS);
            if (coll) {
                stdx::unique_lock<Client> lk(*opCtx->getClient());
                progress.set(lk,
                             CurOp::get(opCtx)->setProgress_inlock(StringData(curOpMessage),
                                                                   coll->numRecords(opCtx)),
                             opCtx);
            } else {
                const auto entry = dbCheckWarningHealthLogEntry(
                    info.nss,
                    "abandoning dbCheck batch because collection no longer exists",
                    OplogEntriesEnum::Batch,
                    Status(ErrorCodes::NamespaceNotFound, "collection not found"));
                HealthLogInterface::get(Client::getCurrent()->getServiceContext())->log(*entry);
                return;
            }
        }

        // Parameters for the hasher.
        auto start = info.start;
        bool reachedEnd = false;

        // Make sure the totals over all of our batches don't exceed the provided limits.
        int64_t totalBytesSeen = 0;
        int64_t totalDocsSeen = 0;

        // Limit the rate of the check.
        using Clock = stdx::chrono::system_clock;
        using TimePoint = stdx::chrono::time_point<Clock>;
        TimePoint lastStart = Clock::now();
        int64_t docsInCurrentInterval = 0;

        do {
            using namespace std::literals::chrono_literals;

            if (Clock::now() - lastStart > 1s) {
                lastStart = Clock::now();
                docsInCurrentInterval = 0;
            }

            auto result =
                _runBatch(opCtx, info, start, info.maxDocsPerBatch, info.maxBytesPerBatch);

            if (_done) {
                return;
            }


            if (!result.isOK()) {
                bool retryable = false;
                std::unique_ptr<HealthLogEntry> entry;

                const auto code = result.getStatus().code();
                if (code == ErrorCodes::LockTimeout) {
                    retryable = true;
                    entry = dbCheckWarningHealthLogEntry(
                        info.nss,
                        "retrying dbCheck batch after timeout due to lock unavailability",
                        OplogEntriesEnum::Batch,
                        result.getStatus());
                } else if (code == ErrorCodes::SnapshotUnavailable) {
                    retryable = true;
                    entry = dbCheckWarningHealthLogEntry(
                        info.nss,
                        "retrying dbCheck batch after conflict with pending catalog operation",
                        OplogEntriesEnum::Batch,
                        result.getStatus());
                } else if (code == ErrorCodes::NamespaceNotFound) {
                    entry = dbCheckWarningHealthLogEntry(
                        info.nss,
                        "abandoning dbCheck batch because collection no longer exists",
                        OplogEntriesEnum::Batch,
                        result.getStatus());
                } else if (code == ErrorCodes::IndexNotFound) {
                    entry = dbCheckWarningHealthLogEntry(
                        info.nss,
                        "skipping dbCheck on collection because it is missing an _id index",
                        OplogEntriesEnum::Batch,
                        result.getStatus());
                } else if (ErrorCodes::isA<ErrorCategory::NotPrimaryError>(code)) {
                    entry = dbCheckWarningHealthLogEntry(
                        info.nss,
                        "stopping dbCheck because node is no longer primary",
                        OplogEntriesEnum::Batch,
                        result.getStatus());
                } else {
                    entry = dbCheckErrorHealthLogEntry(info.nss,
                                                       "dbCheck batch failed",
                                                       OplogEntriesEnum::Batch,
                                                       result.getStatus());
                }
                HealthLogInterface::get(opCtx)->log(*entry);
                if (retryable) {
                    continue;
                }
                return;
            }

            const auto stats = result.getValue();

            _batchesProcessed++;
            auto entry = dbCheckBatchEntry(info.nss,
                                           stats.nDocs,
                                           stats.nBytes,
                                           stats.md5,
                                           stats.md5,
                                           start,
                                           stats.lastKey,
                                           stats.readTimestamp,
                                           stats.time);
            if (kDebugBuild || entry->getSeverity() != SeverityEnum::Info ||
                (_batchesProcessed % gDbCheckHealthLogEveryNBatches.load() == 0)) {
                // On debug builds, health-log every batch result; on release builds, health-log
                // every N batches.
                HealthLogInterface::get(opCtx)->log(*entry);
            }

            WriteConcernResult unused;
            auto status = waitForWriteConcern(opCtx, stats.time, info.writeConcern, &unused);
            if (!status.isOK()) {
                auto entry = dbCheckWarningHealthLogEntry(info.nss,
                                                          "dbCheck failed waiting for writeConcern",
                                                          OplogEntriesEnum::Batch,
                                                          status);
                HealthLogInterface::get(opCtx)->log(*entry);
            }

            start = stats.lastKey;

            // Update our running totals.
            totalDocsSeen += stats.nDocs;
            totalBytesSeen += stats.nBytes;
            docsInCurrentInterval += stats.nDocs;
            {
                stdx::unique_lock<Client> lk(*opCtx->getClient());
                progress.get(lk)->hit(stats.nDocs);
            }

            // Check if we've exceeded any limits.
            bool reachedLast = stats.lastKey >= info.end;
            bool tooManyDocs = totalDocsSeen >= info.maxCount;
            bool tooManyBytes = totalBytesSeen >= info.maxSize;
            reachedEnd = reachedLast || tooManyDocs || tooManyBytes;

            if (docsInCurrentInterval > info.maxRate && info.maxRate > 0) {
                // If an extremely low max rate has been set (substantially smaller than the batch
                // size) we might want to sleep for multiple seconds between batches.
                int64_t timesExceeded = docsInCurrentInterval / info.maxRate;

                stdx::this_thread::sleep_for(timesExceeded * 1s - (Clock::now() - lastStart));
            }
        } while (!reachedEnd);

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            progress.get(lk)->finished();
        }
    }

    /**
     * For organizing the results of batches.
     */
    struct BatchStats {
        int64_t nDocs;
        int64_t nBytes;
        BSONKey lastKey;
        std::string md5;
        repl::OpTime time;
        boost::optional<Timestamp> readTimestamp;
    };

    // Set if the job cannot proceed.
    bool _done;
    std::string _dbName;
    std::unique_ptr<DbCheckRun> _run;

    StatusWith<BatchStats> _runBatch(OperationContext* opCtx,
                                     const DbCheckCollectionInfo& info,
                                     const BSONKey& first,
                                     int64_t batchDocs,
                                     int64_t batchBytes) {
        // Each batch will read at the latest no-overlap point, which is the all_durable timestamp
        // on primaries. We assume that the history window on secondaries is always longer than the
        // time it takes between starting and replicating a batch on the primary. Otherwise, the
        // readTimestamp will not be available on a secondary by the time it processes the oplog
        // entry.
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);

        // dbCheck writes to the oplog, so we need to take an IX lock. We don't need to write to the
        // collection, however, so we only take an intent lock on it.
        Lock::GlobalLock glob(opCtx, MODE_IX);

        // The CollectionCatalog to use for lock-free reads with point-in-time catalog lookups.
        std::shared_ptr<const CollectionCatalog> catalog;

        boost::optional<AutoGetCollection> autoColl;
        const Collection* collection = nullptr;
        // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
        if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
            // Make sure we get a CollectionCatalog in sync with our snapshot.
            catalog = getConsistentCatalogAndSnapshot(opCtx);

            collection = catalog->establishConsistentCollection(
                opCtx,
                {info.nss.db(), info.uuid},
                opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx));
        } else {
            autoColl.emplace(opCtx, info.nss, MODE_IS);
            collection = autoColl->getCollection().get();
        }

        if (_stepdownHasOccurred(opCtx, info.nss)) {
            _done = true;
            return Status(ErrorCodes::PrimarySteppedDown, "dbCheck terminated due to stepdown");
        }

        if (!collection) {
            const auto msg = "Collection under dbCheck no longer exists";
            return {ErrorCodes::NamespaceNotFound, msg};
        }

        auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
        uassert(ErrorCodes::SnapshotUnavailable,
                "No snapshot available yet for dbCheck",
                readTimestamp);
        auto minVisible = collection->getMinimumVisibleSnapshot();
        if (minVisible && *readTimestamp < *collection->getMinimumVisibleSnapshot()) {
            // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
            invariant(!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe());
            return {ErrorCodes::SnapshotUnavailable,
                    str::stream() << "Unable to read from collection " << info.nss
                                  << " due to pending catalog changes"};
        }

        // The CollectionPtr needs to outlive the DbCheckHasher as it's used internally.
        const CollectionPtr collectionPtr(collection);

        boost::optional<DbCheckHasher> hasher;
        try {
            hasher.emplace(opCtx,
                           collectionPtr,
                           first,
                           info.end,
                           std::min(batchDocs, info.maxCount),
                           std::min(batchBytes, info.maxSize));
        } catch (const DBException& e) {
            return e.toStatus();
        }

        const auto batchDeadline = Date_t::now() + Milliseconds(info.maxBatchTimeMillis);
        Status status = hasher->hashAll(opCtx, batchDeadline);

        if (!status.isOK()) {
            return status;
        }

        std::string md5 = hasher->total();

        DbCheckOplogBatch batch;
        batch.setType(OplogEntriesEnum::Batch);
        batch.setNss(info.nss);
        batch.setMd5(md5);
        batch.setMinKey(first);
        batch.setMaxKey(BSONKey(hasher->lastKey()));
        batch.setReadTimestamp(readTimestamp);

        // Send information on this batch over the oplog.
        BatchStats result;
        result.time = _logOp(opCtx, info.nss, collection->uuid(), batch.toBSON());
        result.readTimestamp = readTimestamp;

        result.nDocs = hasher->docsSeen();
        result.nBytes = hasher->bytesSeen();
        result.lastKey = hasher->lastKey();
        result.md5 = md5;
        return result;
    }

    /**
     * Return `true` iff the primary the check is running on has stepped down.
     */
    bool _stepdownHasOccurred(OperationContext* opCtx, const NamespaceString& nss) {
        Status status = opCtx->checkForInterruptNoAssert();

        if (!status.isOK()) {
            return true;
        }

        auto coord = repl::ReplicationCoordinator::get(opCtx);

        if (!coord->canAcceptWritesFor(opCtx, nss)) {
            return true;
        }

        return false;
    }

    // Cumulative number of batches processed. Can wrap around; it's not guaranteed to be in
    // lockstep with other replica set members.
    unsigned int _batchesProcessed = 0;
};

/**
 * The command, as run on the primary.
 */
class DbCheckCmd : public BasicCommand {
public:
    DbCheckCmd() : BasicCommand("dbCheck") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool maintenanceOk() const override {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Validate replica set consistency.\n"
               "Invoke with { dbCheck: <collection name/uuid>,\n"
               "              minKey: <first key, exclusive>,\n"
               "              maxKey: <last key, inclusive>,\n"
               "              maxCount: <try to keep a batch within maxCount number of docs>,\n"
               "              maxSize: <try to keep a batch withing maxSize of docs (bytes)>,\n"
               "              maxCountPerSecond: <max rate in docs/sec>\n"
               "              maxDocsPerBatch: <max number of docs/batch>\n"
               "              maxBytesPerBatch: <try to keep a batch within max bytes/batch>\n"
               "              maxBatchTimeMillis: <max time processing a batch in milliseconds>\n"
               "to check a collection.\n"
               "Invoke with {dbCheck: 1} to check all collections in the database.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        const bool isAuthorized = AuthorizationSession::get(opCtx->getClient())
                                      ->isAuthorizedForActionsOnResource(
                                          ResourcePattern::forAnyResource(), ActionType::dbCheck);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    virtual bool run(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        auto job = getRun(opCtx, dbName, cmdObj);
        try {
            (new DbCheckJob(dbName, std::move(job)))->go();
        } catch (const DBException& e) {
            result.append("ok", false);
            result.append("err", e.toString());
            return false;
        }
        result.append("ok", true);
        return true;
    }
} dbCheckCmd;

}  // namespace
}  // namespace mongo
