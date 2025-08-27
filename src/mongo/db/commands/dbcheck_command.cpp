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


#include "mongo/db/commands/dbcheck_command.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/health_log_gen.h"
#include "mongo/db/local_catalog/health_log_interface.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <ratio>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand
MONGO_FAIL_POINT_DEFINE(hangBeforeExtraIndexKeysCheck);
MONGO_FAIL_POINT_DEFINE(hangBeforeReverseLookupCatalogSnapshot);
MONGO_FAIL_POINT_DEFINE(hangAfterReverseLookupCatalogSnapshot);
MONGO_FAIL_POINT_DEFINE(hangBeforeExtraIndexKeysHashing);
MONGO_FAIL_POINT_DEFINE(primaryHangAfterExtraIndexKeysHashing);

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeDbCheckLogOp);
MONGO_FAIL_POINT_DEFINE(hangBeforeProcessingDbCheckRun);
MONGO_FAIL_POINT_DEFINE(hangBeforeProcessingFirstBatch);
MONGO_FAIL_POINT_DEFINE(hangBeforeAddingDBCheckBatchToOplog);

namespace {
// Makes sure that only one dbcheck operation is running at a time.
stdx::mutex _dbCheckMutex;

// Queue for dbcheck commands that are waiting to be run. There can be at most
// `gDbCheckMaxRunsOnQueue` (default 5) dbchecks in the queue, including 1 currently running. An
// error health log entry will be generated when a dbcheck command is issued when the queue is full
// and that dbcheck will not be added to the queue. Only the first dbcheck on the queue will be
// running.
std::deque<boost::optional<DbCheckCollectionInfo>> _dbChecksInProgress;

// This is waited upon if there is found to already be a dbcheck command running, as
// _dbchecksInProgress would indicate. This is signaled when a dbcheck command finishes.
stdx::condition_variable _dbCheckNotifier;
}  // namespace

// The optional `tenantIdForStartStop` is used for dbCheckStart/dbCheckStop oplog entries so that
// the namespace is still the admin command namespace but the tenantId will be set using the
// namespace that dbcheck is running for.
// Returns an error status if we threw an exception while logging. This may include primary stepdown
// errors. This will acquire the global lock in IX mode.
StatusWith<repl::OpTime> _logOp(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const boost::optional<TenantId>& tenantIdForStartStop,
                                const boost::optional<UUID>& uuid,
                                const BSONObj& obj) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
    oplogEntry.setNss(nss);
    oplogEntry.setTid(nss.tenantId() ? nss.tenantId() : tenantIdForStartStop);
    oplogEntry.setUuid(uuid);
    oplogEntry.setObject(obj);
    try {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        return writeConflictRetry(
            opCtx, "dbCheck oplog entry", NamespaceString::kRsOplogNamespace, [&] {
                auto& clockSource = opCtx->fastClockSource();
                oplogEntry.setWallClockTime(clockSource.now());

                WriteUnitOfWork uow(opCtx);
                repl::OpTime result = repl::logOp(opCtx, &oplogEntry);
                uow.commit();
                return result;
            });
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

/**
 * Initializes currentOp for dbcheck background job.
 */
void _initializeCurOp(OperationContext* opCtx, boost::optional<DbCheckCollectionInfo> info) {
    if (!info) {
        return;
    }

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    auto curOp = CurOp::get(opCtx);
    curOp->setNS(lk, info->nss);
    curOp->setOpDescription(lk, info->toBSON());
    curOp->ensureStarted();
}

/**
 * Returns the default write concern if 'batchWriteConcern' is not set.
 */
WriteConcernOptions _getBatchWriteConcern(
    OperationContext* opCtx,
    const boost::optional<WriteConcernOptions>& providedBatchWriteConcern) {
    // Default constructor: {w:1, wtimeout: 0}.
    WriteConcernOptions batchWriteConcern;
    if (providedBatchWriteConcern) {
        batchWriteConcern = providedBatchWriteConcern.value();
    } else {
        auto wcDefault =
            ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx).getDefaultWriteConcern();
        if (wcDefault) {
            batchWriteConcern = wcDefault.value();
        }
    }

    return batchWriteConcern;
}

BSONObj DbCheckCollectionInfo::toBSON() const {
    BSONObjBuilder builder;
    builder.append("dbcheck",
                   NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    uuid.appendToBuilder(&builder, "uuid");
    if (secondaryIndexCheckParameters) {
        builder.append("secondaryIndexCheckParameters", secondaryIndexCheckParameters->toBSON());
    }

    builder.append("start", start);
    builder.append("end", end);
    builder.append("maxDocsPerBatch", maxDocsPerBatch);
    builder.append("maxBatchTimeMillis", maxBatchTimeMillis);
    builder.append("maxCount", maxCount);
    builder.append("maxSize", maxSize);
    builder.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());
    return builder.obj();
}

DbCheckStartAndStopLogger::DbCheckStartAndStopLogger(OperationContext* opCtx,
                                                     boost::optional<DbCheckCollectionInfo> info)
    : _info(info), _opCtx(opCtx) {
    DbCheckOplogStartStop oplogEntry;
    const auto nss = NamespaceString::kAdminCommandNamespace;
    oplogEntry.setNss(nss);
    oplogEntry.setType(OplogEntriesEnum::Start);

    auto healthLogEntry = dbCheckHealthLogEntry(boost::none /*parameters*/,
                                                boost::none /*nss*/,
                                                boost::none /*collectionUUID*/,
                                                SeverityEnum::Info,
                                                "",
                                                ScopeEnum::Cluster,
                                                OplogEntriesEnum::Start,
                                                boost::none /*data*/);

    // The namespace logged in the oplog entry is the admin command namespace, but the
    // namespace this dbcheck invocation is run on will be stored in the `o.dbCheck` field
    // and in the health log.
    boost::optional<TenantId> tenantId;
    if (_info && _info.value().secondaryIndexCheckParameters) {
        oplogEntry.setSecondaryIndexCheckParameters(
            _info.value().secondaryIndexCheckParameters.value());
        healthLogEntry->setData(BSON(
            "dbCheckParameters" << _info.value().secondaryIndexCheckParameters.value().toBSON()));

        oplogEntry.setNss(_info.value().nss);
        healthLogEntry->setNss(_info.value().nss);

        oplogEntry.setUuid(_info.value().uuid);
        healthLogEntry->setCollectionUUID(_info.value().uuid);

        if (_info && _info.value().nss.tenantId()) {
            tenantId = _info.value().nss.tenantId();
        }
    }

    HealthLogInterface::get(_opCtx)->log(*healthLogEntry);
    auto status = _logOp(_opCtx, nss, tenantId, boost::none /*uuid*/, oplogEntry.toBSON());
    if (!status.isOK()) {
        LOGV2(6202200, "Could not add start event to oplog", "error"_attr = status.getStatus());
    }
}

DbCheckStartAndStopLogger::~DbCheckStartAndStopLogger() {
    DbCheckOplogStartStop oplogEntry;
    const auto nss = NamespaceString::kAdminCommandNamespace;
    oplogEntry.setNss(nss);
    oplogEntry.setType(OplogEntriesEnum::Stop);

    auto healthLogEntry = dbCheckHealthLogEntry(boost::none /*parameters*/,
                                                boost::none /*nss*/,
                                                boost::none /*collectionUUID*/,
                                                SeverityEnum::Info,
                                                "",
                                                ScopeEnum::Cluster,
                                                OplogEntriesEnum::Stop,
                                                boost::none /*data*/);

    // The namespace logged in the oplog entry is the admin command namespace, but the
    // namespace this dbcheck invocation is run on will be stored in the `o.dbCheck` field
    // and in the health log.
    boost::optional<TenantId> tenantId;
    if (_info && _info.value().secondaryIndexCheckParameters) {
        oplogEntry.setSecondaryIndexCheckParameters(
            _info.value().secondaryIndexCheckParameters.value());
        healthLogEntry->setData(BSON(
            "dbCheckParameters" << _info.value().secondaryIndexCheckParameters.value().toBSON()));

        oplogEntry.setNss(_info.value().nss);
        healthLogEntry->setNss(_info.value().nss);

        oplogEntry.setUuid(_info.value().uuid);
        healthLogEntry->setCollectionUUID(_info.value().uuid);

        if (_info && _info.value().nss.tenantId()) {
            tenantId = _info.value().nss.tenantId();
        }
    }

    // We should log to the health log before writing to the oplog, as writing to the oplog
    // during stepdown will raise an exception. We want to ensure that we log the dbcheck stop
    // on stepdown.
    HealthLogInterface::get(_opCtx)->log(*healthLogEntry);
    auto status = _logOp(_opCtx, nss, tenantId, boost::none /*uuid*/, oplogEntry.toBSON());
    if (!status.isOK()) {
        LOGV2(6202201, "Could not add stop event to oplog", "error"_attr = status.getStatus());
    }
}

std::unique_ptr<DbCheckRun> singleCollectionRun(OperationContext* opCtx,
                                                const DatabaseName& dbName,
                                                const DbCheckSingleInvocation& invocation) {
    const auto gSecondaryIndexChecksInDbCheck =
        repl::feature_flags::gSecondaryIndexChecksInDbCheck.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    if (!gSecondaryIndexChecksInDbCheck) {
        uassert(ErrorCodes::InvalidOptions,
                "When featureFlagSecondaryIndexChecksInDbCheck is not enabled, the validateMode "
                "parameter cannot be set.",
                !invocation.getValidateMode());
    } else {
        if (invocation.getValidateMode() == mongo::DbCheckValidationModeEnum::extraIndexKeysCheck) {
            uassert(ErrorCodes::InvalidOptions,
                    "When validateMode is set to extraIndexKeysCheck, the secondaryIndex parameter "
                    "must be set.",
                    invocation.getSecondaryIndex());
        } else {
            uassert(ErrorCodes::InvalidOptions,
                    "When validateMode is set to dataConsistency or "
                    "dataConsistencyAndMissingIndexKeysCheck, the secondaryIndex parameter cannot "
                    "be set.",
                    !invocation.getSecondaryIndex());
            uassert(ErrorCodes::InvalidOptions,
                    "When validateMode is set to dataConsistency or "
                    "dataConsistencyAndMissingIndexKeysCheck, the skipLookupForExtraKeys parameter "
                    "cannot be set.",
                    !invocation.getSkipLookupForExtraKeys());
        }
    }
    NamespaceString nss(NamespaceStringUtil::deserialize(dbName, invocation.getColl()));

    boost::optional<UUID> uuid;
    try {
        const auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);

        uassert(ErrorCodes::NamespaceNotFound,
                "Collection " + invocation.getColl() + " not found",
                coll.exists());
        uuid = coll.uuid();
    } catch (ExceptionFor<ErrorCodes::CommandNotSupportedOnView>& ex) {
        // Collection acquisition fails with 'CommandNotSupportedOnView' if the namespace is
        // referring to a view.
        ex.addContext(invocation.getColl() + " is a view hence 'dbcheck' is not supported.");
        throw;
    }

    uassert(40619,
            "Cannot run dbCheck on " + nss.toStringForErrorMsg() + " because it is not replicated",
            nss.isReplicated());

    uassert(6769500, "dbCheck no longer supports snapshotRead:false", invocation.getSnapshotRead());

    BSONObj start;
    BSONObj end;
    const auto maxCount = invocation.getMaxCount();
    const auto maxSize = invocation.getMaxSize();
    const auto maxDocsPerBatch = invocation.getMaxDocsPerBatch();
    const auto maxBatchTimeMillis = invocation.getMaxBatchTimeMillis();

    boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParameters = boost::none;
    if (gSecondaryIndexChecksInDbCheck) {
        secondaryIndexCheckParameters = SecondaryIndexCheckParameters();
        secondaryIndexCheckParameters->setSkipLookupForExtraKeys(
            invocation.getSkipLookupForExtraKeys());
        if (invocation.getValidateMode()) {
            secondaryIndexCheckParameters->setValidateMode(invocation.getValidateMode().value());
        }

        StringData indexName = "_id";
        if (invocation.getSecondaryIndex()) {
            secondaryIndexCheckParameters->setSecondaryIndex(
                invocation.getSecondaryIndex().value());
            indexName = invocation.getSecondaryIndex().value();
        }

        if (invocation.getBsonValidateMode()) {
            secondaryIndexCheckParameters->setBsonValidateMode(
                invocation.getBsonValidateMode().value());
        }

        // TODO SERVER-78399: Remove special handling start/end being optional once feature flag is
        // removed.

        // If start is not set, or is the default value of kMinBSONKey, set to {_id: MINKEY} or
        // {<indexName>: MINKEY}. Otherwise, set it to the passed in value.
        if (!invocation.getStart() ||
            SimpleBSONObjComparator::kInstance.evaluate(invocation.getStart().get() ==
                                                        kMinBSONKey)) {
            // MINKEY is { "$minKey" : 1 }.
            start = BSON(indexName << MINKEY);
        } else {
            start = invocation.getStart().get().copy();
        }

        if (!invocation.getEnd() ||
            SimpleBSONObjComparator::kInstance.evaluate(invocation.getEnd().get() == kMaxBSONKey)) {
            // MAXKEY is { "$maxKey" : 1 }.
            end = BSON(indexName << MAXKEY);
        } else {
            end = invocation.getEnd().get().copy();
        }
    } else {
        start = invocation.getMinKey().obj();
        end = invocation.getMaxKey().obj();
    }

    const auto info =
        DbCheckCollectionInfo{nss,
                              uuid.get(),
                              start,
                              end,
                              maxCount,
                              maxSize,
                              maxDocsPerBatch,
                              maxBatchTimeMillis,
                              _getBatchWriteConcern(opCtx, invocation.getBatchWriteConcern()),
                              secondaryIndexCheckParameters,
                              {opCtx, [&]() {
                                   return gMaxDbCheckMBperSec.load();
                               }}};
    auto result = std::make_unique<DbCheckRun>();
    result->push_back(info);
    return result;
}

std::unique_ptr<DbCheckRun> fullDatabaseRun(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const DbCheckAllInvocation& invocation) {
    uassert(
        ErrorCodes::InvalidNamespace, "Cannot run dbCheck on local database", !dbName.isLocalDB());

    AutoGetDb agd(opCtx, dbName, MODE_IS);
    uassert(ErrorCodes::NamespaceNotFound,
            "Database " + dbName.toStringForErrorMsg() + " not found",
            agd.getDb());

    uassert(6769501, "dbCheck no longer supports snapshotRead:false", invocation.getSnapshotRead());

    const int64_t max = std::numeric_limits<int64_t>::max();
    const auto maxDocsPerBatch = invocation.getMaxDocsPerBatch();
    const auto maxBatchTimeMillis = invocation.getMaxBatchTimeMillis();
    auto result = std::make_unique<DbCheckRun>();
    auto perCollectionWork = [&](const Collection* coll) {
        if (!coll->ns().isReplicated()) {
            return true;
        }
        DbCheckCollectionInfo info{coll->ns(),
                                   coll->uuid(),
                                   BSON("_id" << MINKEY),
                                   BSON("_id" << MAXKEY),
                                   max,
                                   max,
                                   maxDocsPerBatch,
                                   maxBatchTimeMillis,
                                   _getBatchWriteConcern(opCtx, invocation.getBatchWriteConcern()),
                                   boost::none,
                                   {opCtx, [&]() {
                                        return gMaxDbCheckMBperSec.load();
                                    }}};
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
    if (toParse["dbCheck"].type() == BSONType::string) {
        return singleCollectionRun(
            opCtx,
            dbName,
            DbCheckSingleInvocation::parse(toParse,
                                           IDLParserContext("",
                                                            auth::ValidatedTenancyScope::get(opCtx),
                                                            dbName.tenantId(),
                                                            SerializationContext::stateDefault())));
    } else {
        // Otherwise, it's the database-wide form.
        return fullDatabaseRun(
            opCtx,
            dbName,
            DbCheckAllInvocation::parse(toParse,
                                        IDLParserContext("",
                                                         auth::ValidatedTenancyScope::get(opCtx),
                                                         dbName.tenantId(),
                                                         SerializationContext::stateDefault())));
    }
}

void DbCheckJob::run() {
    // Every dbCheck runs in its own client.
    ThreadClient tc(name(), _service);
    auto uniqueOpCtx = tc->makeOperationContext();
    auto opCtx = uniqueOpCtx.get();

    // DBcheck only acquires the collection in a lock-free read mode, which takes the global IS
    // lock for most of its work. It only acquires the globalLock and RSTL
    // in IX mode when writing to the oplog. Therefore, it doesn't prevent stepdown most of the
    // time. However, it's crucial to ensure that this operation will be terminated by the
    // RstlKillOpThread during stepdown, as we don't want the background thread to continue running
    // after the node becomes secondary.
    // It's safe if stepdown occurs before marking the operation as interruptible because we log
    // 'dbcheckStart' to the oplog at the beginning. Therefore, even if a stepdown happens early,
    // the operation fails at logging 'dbcheckStart', and no further work is done.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // DbCheckRun will be empty in a fullDatabaseRun where all collections are not replicated.
    // TODO SERVER-79132: Remove this logic once dbCheck no longer allows for a full database
    // run
    boost::optional<DbCheckCollectionInfo> info = boost::none;
    if (!_run->empty()) {
        info = _run->front();
    }

    // TODO SERVER-78399: Clean up this check once feature flag is removed.
    // Only one dbcheck operation can be in progress.
    if (info && info.value().secondaryIndexCheckParameters) {
        stdx::unique_lock<stdx::mutex> lock(_dbCheckMutex);

        size_t queueCapacity = gDbCheckMaxRunsOnQueue.load();
        if (_dbChecksInProgress.size() >= queueCapacity) {
            std::vector<BSONObj> runsInQueue;
            for (size_t i = 0; i < _dbChecksInProgress.size(); i++) {
                runsInQueue.push_back(_dbChecksInProgress.at(i).get().toBSON());
            }

            std::unique_ptr<HealthLogEntry> logEntry =
                dbCheckHealthLogEntry(info->secondaryIndexCheckParameters,
                                      boost::none,
                                      boost::none,
                                      SeverityEnum::Error,
                                      "too many dbcheck runs in queue",
                                      ScopeEnum::Database,
                                      OplogEntriesEnum::Batch,
                                      BSON("dbCheckQueue" << runsInQueue));
            HealthLogInterface::get(opCtx)->log(*logEntry);
            return;
        }

        _dbChecksInProgress.push_back(info);
        // Wait for the dbcheck job this thread enqueued to be at the front of the queue.
        // Once it is at the front, we can start that job.
        try {
            opCtx->waitForConditionOrInterrupt(_dbCheckNotifier, lock, [&] {
                return _dbChecksInProgress.front().get().toBSON().toString().compare(
                           info.get().toBSON().toString()) == 0;
            });
        } catch (const DBException& ex) {
            // Catch interrupt exceptions. Log a health log entry and return in this case.
            auto errCode = ex.code();
            std::unique_ptr<HealthLogEntry> logEntry;
            if (ErrorCodes::isA<ErrorCategory::NotPrimaryError>(errCode)) {
                logEntry = dbCheckWarningHealthLogEntry(
                    info->secondaryIndexCheckParameters,
                    info->nss,
                    info->uuid,
                    "abandoning dbCheck batch due to stepdown",
                    ScopeEnum::Collection,
                    OplogEntriesEnum::Batch,
                    Status(ErrorCodes::PrimarySteppedDown, "dbCheck terminated due to stepdown"));
            } else {
                logEntry = dbCheckErrorHealthLogEntry(info->secondaryIndexCheckParameters,
                                                      info->nss,
                                                      info->uuid,
                                                      "dbCheck failed",
                                                      ScopeEnum::Cluster,
                                                      OplogEntriesEnum::Batch,
                                                      ex.toStatus());
            }
            HealthLogInterface::get(opCtx)->log(*logEntry);
            return;
        }
    }

    ON_BLOCK_EXIT([&] {
        // TODO SERVER-78399: Clean up this check once feature flag is removed.
        if (info && info.value().secondaryIndexCheckParameters) {
            // The thread holds onto the _dbcheckMutex and waits for its dbcheck job to be at the
            // front of the queue. The `waitForConditionOrInterrupt` releases the lock so we
            // shouldn't run into a deadlock here.
            stdx::lock_guard<stdx::mutex> lock(_dbCheckMutex);
            _dbChecksInProgress.pop_front();
            _dbCheckNotifier.notify_all();
        }
    });

    DbCheckStartAndStopLogger startStop(opCtx, info);
    _initializeCurOp(opCtx, info);
    ON_BLOCK_EXIT([opCtx] { CurOp::get(opCtx)->done(); });

    if (MONGO_unlikely(hangBeforeProcessingDbCheckRun.shouldFail())) {
        LOGV2(7949000, "Hanging dbcheck due to failpoint 'hangBeforeProcessingDbCheckRun'");
        hangBeforeProcessingDbCheckRun.pauseWhileSet();
    }

    for (const auto& coll : *_run) {
        DbChecker dbChecker(coll);
        dbChecker.doCollection(opCtx);
    }
}

void DbChecker::doCollection(OperationContext* opCtx) noexcept {
    // TODO SERVER-78399: Clean up this check once feature flag is removed.
    try {
        boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParameters =
            _info.secondaryIndexCheckParameters;
        if (secondaryIndexCheckParameters) {
            mongo::DbCheckValidationModeEnum validateMode =
                secondaryIndexCheckParameters.get().getValidateMode();
            switch (validateMode) {
                case mongo::DbCheckValidationModeEnum::extraIndexKeysCheck: {
                    _extraIndexKeysCheck(opCtx);
                    return;
                }
                case mongo::DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck:
                case mongo::DbCheckValidationModeEnum::dataConsistency:
                    // _dataConsistencyCheck will check whether to do missingIndexKeysCheck.
                    _dataConsistencyCheck(opCtx);
                    return;
            }
            MONGO_UNREACHABLE;
        } else {
            _dataConsistencyCheck(opCtx);
        }
    } catch (const DBException& e) {
        auto errCode = e.code();
        std::unique_ptr<HealthLogEntry> logEntry;
        if (ErrorCodes::isA<ErrorCategory::NotPrimaryError>(errCode)) {
            logEntry = dbCheckWarningHealthLogEntry(_info.secondaryIndexCheckParameters,
                                                    _info.nss,
                                                    _info.uuid,
                                                    "abandoning dbCheck batch due to stepdown",
                                                    ScopeEnum::Collection,
                                                    OplogEntriesEnum::Batch,
                                                    e.toStatus());
        } else {
            logEntry = dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                                  _info.nss,
                                                  _info.uuid,
                                                  "dbCheck failed with an uncaught exception",
                                                  ScopeEnum::Cluster,
                                                  OplogEntriesEnum::Batch,
                                                  e.toStatus());
        }
        HealthLogInterface::get(opCtx)->log(*logEntry);
    }
}

void DbChecker::_extraIndexKeysCheck(OperationContext* opCtx) {
    if (MONGO_unlikely(hangBeforeExtraIndexKeysCheck.shouldFail())) {
        LOGV2_DEBUG(7844908, 3, "Hanging due to hangBeforeExtraIndexKeysCheck failpoint");
        hangBeforeExtraIndexKeysCheck.pauseWhileSet(opCtx);
    }
    StringData indexName = _info.secondaryIndexCheckParameters.get().getSecondaryIndex();

    // TODO SERVER-79846: Add testing for progress meter
    // ProgressMeterHolder progress;

    // If this value is not boost::none, it should be a keystring with a recordId appended.
    boost::optional<key_string::Value> nextKeyToSeekWithRecordId = boost::none;
    bool reachedEnd = false;

    int64_t totalBytesSeen = 0;
    int64_t totalKeysSeen = 0;
    do {
        DbCheckExtraIndexKeysBatchStats batchStats = {};
        batchStats.deadline = Date_t::now() + Milliseconds(_info.maxBatchTimeMillis);
        batchStats.nConsecutiveIdenticalKeysAtEnd = 1;

        // 1. Get batch bounds (stored in batchStats) and run reverse lookup if
        // skipLookupForExtraKeys is not set.
        Status reverseLookupStatus = _getExtraIndexKeysBatchAndRunReverseLookup(
            opCtx, indexName, nextKeyToSeekWithRecordId, batchStats);
        if (!reverseLookupStatus.isOK()) {
            LOGV2_DEBUG(7844901,
                        3,
                        "abandoning extra index keys check because of error with batching and "
                        "reverse lookup",
                        "status"_attr = reverseLookupStatus.reason(),
                        "indexName"_attr = indexName,
                        logAttrs(_info.nss),
                        "uuid"_attr = _info.uuid);
            break;
        }

        // 2. Get the actual last keystring processed from reverse lookup.
        // If the first or last key of the batch is not initialized, that means there was an error
        // with batching.
        if (batchStats.batchStartWithRecordId.isEmpty() ||
            batchStats.batchEndWithRecordId.isEmpty() ||
            batchStats.batchStartBsonWithoutRecordId.isEmpty() ||
            batchStats.batchEndBsonWithoutRecordId.isEmpty()) {
            LOGV2_DEBUG(7844903,
                        3,
                        "abandoning extra index keys check because of error with batching",
                        "indexName"_attr = indexName,
                        logAttrs(_info.nss),
                        "uuid"_attr = _info.uuid);
            Status status = Status(ErrorCodes::NoSuchKey,
                                   "could not create batch bounds because of error while batching");
            const auto logEntry = dbCheckErrorHealthLogEntry(
                _info.secondaryIndexCheckParameters,
                _info.nss,
                _info.uuid,
                "abandoning dbCheck extra index keys check because of error with batching",
                ScopeEnum::Index,
                OplogEntriesEnum::Batch,
                status);
            HealthLogInterface::get(opCtx)->log(*logEntry);
            break;
        }

        // 3. Run hashing algorithm.
        Status hashStatus = _hashExtraIndexKeysCheck(opCtx, &batchStats);
        if (MONGO_unlikely(primaryHangAfterExtraIndexKeysHashing.shouldFail())) {
            LOGV2_DEBUG(
                3083201, 3, "Hanging due to primaryHangAfterExtraIndexKeysHashing failpoint");
            primaryHangAfterExtraIndexKeysHashing.pauseWhileSet(opCtx);
        }
        if (!hashStatus.isOK()) {
            LOGV2_DEBUG(7844902,
                        3,
                        "abandoning extra index keys check because of error with hashing",
                        "status"_attr = hashStatus.reason(),
                        "indexName"_attr = indexName,
                        logAttrs(_info.nss),
                        "uuid"_attr = _info.uuid);
            break;
        }

        // 4. Update nextKeyToSeekWithRecordId to the index key after batchEnd.
        nextKeyToSeekWithRecordId = batchStats.nextKeyToBeCheckedWithRecordId;

        // TODO SERVER-79846: Add testing for progress meter
        // {
        //     stdx::unique_lock<Client> lk(*opCtx->getClient());
        //     progress.get(lk)->hit(batchStats.nDocs);
        // }

        // 5. Check if we've exceeded any limits.
        totalBytesSeen += batchStats.nBytes;
        totalKeysSeen += batchStats.nKeys;

        bool tooManyKeys = totalKeysSeen >= _info.maxCount;
        bool tooManyBytes = totalBytesSeen >= _info.maxSize;
        reachedEnd = batchStats.finishedIndexCheck || tooManyKeys || tooManyBytes;
    } while (!reachedEnd);

    // TODO SERVER-79846: Add testing for progress meter
    // {
    //     stdx::unique_lock<Client> lk(*opCtx->getClient());
    //     progress.get(lk)->finished();
    // }
}

bool DbChecker::_shouldRetryExtraKeysCheck(OperationContext* opCtx,
                                           Status status,
                                           DbCheckExtraIndexKeysBatchStats* batchStats,
                                           int numRetries) const {
    uassert(7985002,
            "status should never be OK if we are debating retrying extra keys check",
            !status.isOK());

    bool isRetryableError = false;
    std::string msg = "";
    bool logError = false;

    auto code = status.code();
    if (code == ErrorCodes::IndexNotFound) {
        msg = "abandoning dbCheck extra index keys check because index no longer exists";
    } else if (code == ErrorCodes::DbCheckAttemptOnClusteredCollectionIdIndex) {
        msg =
            "skipping dbCheck extra index keys check on the _id index because the target "
            "collection is a clustered collection that doesn't have an _id index";
    } else if (code == ErrorCodes::NamespaceNotFound) {
        msg = "abandoning dbCheck extra index keys check because collection no longer exists";
    } else if (code == ErrorCodes::IndexIsEmpty) {
        msg =
            "abandoning dbCheck extra index keys check because there are no keys left in the index";
    } else if (code == ErrorCodes::IndexOptionsConflict) {
        // TODO (SERVER-83074): Enable special indexes in dbcheck and remove this check.
        msg = "abandoning dbCheck extra index keys check because index type is not supported";
    } else if (code == ErrorCodes::ObjectIsBusy) {
        isRetryableError = true;
        msg = "stopping dbCheck extra keys batch because a resource is in use";
    } else if (code == ErrorCodes::CommandNotSupportedOnView) {
        msg =
            "abandoning dbCheck batch because collection no longer exists, but there is a view "
            "with the identical name";
    } else if (ErrorCodes::isA<ErrorCategory::NotPrimaryError>(code)) {
        msg = "abandoning dbCheck batch due to stepdown";
    } else {
        msg = "stopping dbCheck extra keys batch due to unexpected error";
        logError = true;
    }

    if (numRetries > repl::dbCheckMaxInternalRetries.load() || !isRetryableError) {
        BSONObjBuilder context;
        _appendContextForLoggingExtraKeysCheck(batchStats, &context);

        std::unique_ptr<HealthLogEntry> entry;
        if (logError) {
            entry = dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                               _info.nss,
                                               _info.uuid,
                                               msg,
                                               ScopeEnum::Collection,
                                               OplogEntriesEnum::Batch,
                                               status,
                                               context.done());
        } else {
            entry = dbCheckWarningHealthLogEntry(_info.secondaryIndexCheckParameters,
                                                 _info.nss,
                                                 _info.uuid,
                                                 msg,
                                                 ScopeEnum::Collection,
                                                 OplogEntriesEnum::Batch,
                                                 status,
                                                 context.done());
        }
        HealthLogInterface::get(opCtx)->log(*entry);

        LOGV2_DEBUG(7985007,
                    3,
                    "not internally retrying dbCheck extra keys check",
                    "status"_attr = status,
                    "msg"_attr = msg,
                    "numRetries"_attr = numRetries);
        return false;
    }

    return true;
}

/**
 * Sets up a hasher and hashes one batch for extra index keys check.
 * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
 */
Status DbChecker::_hashExtraIndexKeysCheck(OperationContext* opCtx,
                                           DbCheckExtraIndexKeysBatchStats* batchStats) {
    if (MONGO_unlikely(hangBeforeExtraIndexKeysHashing.shouldFail())) {
        LOGV2_DEBUG(7844906, 3, "Hanging due to hangBeforeExtraIndexKeysHashing failpoint");
        hangBeforeExtraIndexKeysHashing.pauseWhileSet(opCtx);
    }

    int numRetriesHashing = 0;
    int64_t sleepMillis = 100;

    auto status = Status::OK();

    do {
        status = _runHashExtraKeyCheck(opCtx, batchStats);
        if (status.isOK()) {
            // On success, break from the loop and continue on to the next batch.
            break;
        }

        if (!_shouldRetryExtraKeysCheck(opCtx, status, batchStats, numRetriesHashing)) {
            // Return immediately if we should stop retrying.
            return status;
        }

        // Increment the amount of time in-between retries.
        sleepMillis *= 2;
        opCtx->sleepFor(Milliseconds(sleepMillis));
        numRetriesHashing++;
        LOGV2_DEBUG(
            7985004, 3, "retrying extra index keys hashing", "numRetries"_attr = numRetriesHashing);
    } while (!status.isOK());

    uassert(7985000,
            "dbcheck hashing extra index keys should be successful if we are logging to healthlog",
            status.isOK());
    auto logEntry = dbCheckBatchHealthLogEntry(
        _info.secondaryIndexCheckParameters,
        batchStats->batchId,
        _info.nss,
        _info.uuid,
        batchStats->nHasherKeys,
        batchStats->nHasherBytes,
        batchStats->md5,
        batchStats->md5,
        key_string::rehydrateKey(batchStats->keyPattern, batchStats->batchStartBsonWithoutRecordId),
        key_string::rehydrateKey(batchStats->keyPattern, batchStats->batchEndBsonWithoutRecordId),
        key_string::rehydrateKey(batchStats->keyPattern,
                                 batchStats->lastBsonCheckedWithoutRecordId),
        batchStats->nHasherConsecutiveIdenticalKeysAtEnd,
        batchStats->readTimestamp,
        batchStats->time,
        boost::none /* collOpts */,
        batchStats->indexSpec);

    if (kDebugBuild || logEntry->getSeverity() != SeverityEnum::Info ||
        batchStats->logToHealthLog) {
        // On debug builds, health-log every batch result.
        HealthLogInterface::get(opCtx)->log(*logEntry);
    }

    WriteConcernResult unused;
    numRetriesHashing = 0;
    sleepMillis = 100;
    while (numRetriesHashing <= repl::dbCheckMaxInternalRetries.load()) {
        status = waitForWriteConcern(opCtx, batchStats->time, _info.writeConcern, &unused);
        if (status.isOK()) {
            return status;
        }

        // Sleep with increasing backoff in-between retries.
        opCtx->sleepFor(Milliseconds(sleepMillis));
        sleepMillis *= 2;

        numRetriesHashing++;
    }

    uassert(7985001,
            "status should be error after failing multiple retries in wait for write concern",
            !status.isOK());
    BSONObjBuilder context;
    _appendContextForLoggingExtraKeysCheck(batchStats, &context);

    auto entry = dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                            _info.nss,
                                            _info.uuid,
                                            "dbCheck failed waiting for writeConcern",
                                            ScopeEnum::Collection,
                                            OplogEntriesEnum::Batch,
                                            status,
                                            context.done());
    HealthLogInterface::get(opCtx)->log(*entry);
    return status;
}

Status DbChecker::_runHashExtraKeyCheck(OperationContext* opCtx,
                                        DbCheckExtraIndexKeysBatchStats* batchStats) {
    StringData indexName = _info.secondaryIndexCheckParameters.get().getSecondaryIndex();
    DbCheckOplogBatch oplogBatch;
    {
        // We need to release the acquisition by dbcheck before writing to the oplog. This is
        // because dbcheck acquires the global lock in IS mode, and the oplog will attempt to
        // acquire the global lock in IX mode. Since we don't allow upgrading the global lock,
        // releasing the lock before writing to the oplog is essential.
        const auto acquisitionSW = _acquireDBCheckLocks(opCtx, _info.nss);
        if (!acquisitionSW.isOK()) {
            batchStats->finishedIndexCheck = true;
            return acquisitionSW.getStatus();
        }

        const CollectionPtr& collection = acquisitionSW.getValue()->collection().getCollectionPtr();

        auto readTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
        uassert(ErrorCodes::SnapshotUnavailable,
                "No snapshot available yet for dbCheck extra index keys check",
                readTimestamp);

        auto indexSW = _acquireIndex(opCtx, collection, indexName);
        if (!indexSW.isOK()) {
            batchStats->finishedIndexCheck = true;
            return indexSW.getStatus();
        }
        auto index = indexSW.getValue();

        // Set the batchStats key pattern and index spec for logging. This should be set already if
        // we ran reverse lookup, but we set it here in case we skipped reverse lookup.
        batchStats->keyPattern = index->keyPattern();
        batchStats->indexSpec = index->infoObj();

        const auto rehydratedBatchStartBsonWithoutRecordId = key_string::rehydrateKey(
            index->keyPattern(), batchStats->batchStartBsonWithoutRecordId);
        const auto rehydratedBatchEndBsonWithoutRecordId =
            key_string::rehydrateKey(index->keyPattern(), batchStats->batchEndBsonWithoutRecordId);
        const auto rehydratedLastBsonCheckedWithoutRecordId = key_string::rehydrateKey(
            index->keyPattern(), batchStats->lastBsonCheckedWithoutRecordId);

        LOGV2_DEBUG(8520000,
                    3,
                    "Beginning hash for extra keys batch",
                    "batchStart"_attr = rehydratedBatchStartBsonWithoutRecordId,
                    "batchEnd"_attr = rehydratedBatchEndBsonWithoutRecordId,
                    "lastKeyChecked"_attr = rehydratedLastBsonCheckedWithoutRecordId);

        // Create hasher.
        boost::optional<DbCheckHasher> hasher;
        try {
            hasher.emplace(opCtx,
                           *acquisitionSW.getValue(),
                           rehydratedBatchStartBsonWithoutRecordId,
                           rehydratedBatchEndBsonWithoutRecordId,
                           _info.secondaryIndexCheckParameters,
                           &_info.dataThrottle,
                           indexName,
                           std::min(_info.maxDocsPerBatch, _info.maxCount),
                           _info.maxSize);
        } catch (const DBException& e) {
            return e.toStatus();
        }


        Status status =
            hasher->hashForExtraIndexKeysCheck(opCtx,
                                               collection.get(),
                                               rehydratedBatchStartBsonWithoutRecordId,
                                               rehydratedBatchEndBsonWithoutRecordId,
                                               rehydratedLastBsonCheckedWithoutRecordId);
        invariant(status.code() != ErrorCodes::DbCheckSecondaryBatchTimeout);

        if (!status.isOK()) {
            return status;
        }

        // Send information on this batch over the oplog.
        std::string md5 = hasher->total();
        oplogBatch.setType(OplogEntriesEnum::Batch);
        oplogBatch.setNss(_info.nss);
        oplogBatch.setReadTimestamp(*readTimestamp);
        oplogBatch.setMd5(md5);
        oplogBatch.setBatchStart(rehydratedBatchStartBsonWithoutRecordId);
        oplogBatch.setBatchEnd(rehydratedBatchEndBsonWithoutRecordId);
        oplogBatch.setLastKeyChecked(rehydratedLastBsonCheckedWithoutRecordId);
        if (_info.secondaryIndexCheckParameters) {
            oplogBatch.setSecondaryIndexCheckParameters(_info.secondaryIndexCheckParameters);
        }

        LOGV2_DEBUG(7844900,
                    3,
                    "hashed one batch on primary",
                    "batchStart"_attr = rehydratedBatchStartBsonWithoutRecordId,
                    "batchEnd"_attr = rehydratedBatchEndBsonWithoutRecordId,
                    "hasherLastKeyChecked"_attr = hasher->lastKeySeen(),
                    "md5"_attr = md5,
                    "keysHashed"_attr = hasher->keysSeen(),
                    "bytesHashed"_attr = hasher->bytesSeen(),
                    "nConsecutiveIdenticalIndexKeysSeenAtEnd"_attr =
                        hasher->nConsecutiveIdenticalIndexKeysSeenAtEnd(),
                    "readTimestamp"_attr = readTimestamp,
                    "indexName"_attr = indexName,
                    logAttrs(_info.nss),
                    "uuid"_attr = _info.uuid);

        auto [shouldLogBatch, batchId] = _shouldLogOplogBatch(oplogBatch);
        batchStats->logToHealthLog = shouldLogBatch;
        batchStats->batchId = batchId;
        batchStats->readTimestamp = readTimestamp;
        batchStats->nHasherKeys = hasher->keysSeen();
        batchStats->nHasherBytes = hasher->bytesSeen();
        batchStats->md5 = md5;
        batchStats->nHasherConsecutiveIdenticalKeysAtEnd =
            hasher->nConsecutiveIdenticalIndexKeysSeenAtEnd();
    }

    if (MONGO_unlikely(hangBeforeAddingDBCheckBatchToOplog.shouldFail())) {
        LOGV2(8831800, "Hanging dbCheck due to failpoint 'hangBeforeAddingDBCheckBatchToOplog'");
        hangBeforeAddingDBCheckBatchToOplog.pauseWhileSet();
    }

    auto opTimeSW = _logOp(
        opCtx, _info.nss, boost::none /* tenantIdForStartStop */, _info.uuid, oplogBatch.toBSON());
    if (!opTimeSW.isOK()) {
        return opTimeSW.getStatus();
    }
    batchStats->time = opTimeSW.getValue();
    return Status::OK();
}


/**
 * Gets batch bounds for extra index keys check and stores the info in batchStats. Runs
 * reverse lookup if skipLookupForExtraKeys is not set.
 * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
 */
Status DbChecker::_getExtraIndexKeysBatchAndRunReverseLookup(
    OperationContext* opCtx,
    StringData indexName,
    const boost::optional<key_string::Value>& nextKeyToSeekWithRecordId,
    DbCheckExtraIndexKeysBatchStats& batchStats) {
    bool reachedBatchEnd = false;
    auto snapshotFirstKeyWithRecordId = nextKeyToSeekWithRecordId;
    int numRetries = 0;
    int64_t sleepMillis = 100;
    do {
        auto status = _getCatalogSnapshotAndRunReverseLookup(
            opCtx, indexName, snapshotFirstKeyWithRecordId, batchStats);
        if (!status.isOK()) {
            LOGV2_DEBUG(7844807,
                        3,
                        "error occurred with reverse lookup",
                        "status"_attr = status.reason(),
                        "indexName"_attr = indexName,
                        logAttrs(_info.nss),
                        "uuid"_attr = _info.uuid);
            if (!_shouldRetryExtraKeysCheck(opCtx, status, &batchStats, numRetries)) {
                return status;
            }
            if (batchStats.finishedIndexBatch || batchStats.finishedIndexCheck) {
                return status;
            }

            // Skip updating snapshotFirstKeyWithRecordId so that we retry the batch starting with
            // the same key if an internal retryable error occurred.
            opCtx->sleepFor(Milliseconds(sleepMillis));
            sleepMillis *= 2;
            numRetries++;
            LOGV2_DEBUG(7985008,
                        3,
                        "retrying extra index keys reverse lookup",
                        "numRetries"_attr = numRetries);
            continue;
        }

        if (MONGO_unlikely(hangAfterReverseLookupCatalogSnapshot.shouldFail())) {
            LOGV2_DEBUG(
                7844810, 3, "Hanging due to hangAfterReverseLookupCatalogSnapshot failpoint");
            hangAfterReverseLookupCatalogSnapshot.pauseWhileSet(opCtx);
        }

        reachedBatchEnd = batchStats.finishedIndexBatch;
        snapshotFirstKeyWithRecordId = batchStats.nextKeyToBeCheckedWithRecordId;
    } while (!reachedBatchEnd && !batchStats.finishedIndexCheck);
    return Status::OK();
}

/**
 * Acquires a consistent catalog snapshot and iterates through the secondary index in order
 * to get the batch bounds. Runs reverse lookup if skipLookupForExtraKeys is not set.
 *
 * We release the snapshot by exiting the function. This occurs when:
 *   * we have finished the whole extra index keys check,
 *   * we have finished one batch
 *   * The number of keys we've looked at has met or exceeded dbCheckMaxTotalIndexKeysPerSnapshot
 *   * if we have identical keys at the end of the batch, one of the above conditions is met and
 *     the number of consecutive identical keys we've looked at has met or exceeded
 *     dbCheckMaxConsecutiveIdenticalIndexKeysPerSnapshot
 *
 * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
 */
Status DbChecker::_getCatalogSnapshotAndRunReverseLookup(
    OperationContext* opCtx,
    StringData indexName,
    const boost::optional<key_string::Value>& snapshotFirstKeyWithRecordId,
    DbCheckExtraIndexKeysBatchStats& batchStats) {
    if (MONGO_unlikely(hangBeforeReverseLookupCatalogSnapshot.shouldFail())) {
        LOGV2_DEBUG(7844804, 3, "Hanging due to hangBeforeReverseLookupCatalogSnapshot failpoint");
        hangBeforeReverseLookupCatalogSnapshot.pauseWhileSet(opCtx);
    }

    Status status = Status::OK();

    // Check that collection and index still exist.
    const auto acquisitionSW = _acquireDBCheckLocks(opCtx, _info.nss);
    if (!acquisitionSW.isOK()) {
        batchStats.finishedIndexBatch = true;
        batchStats.finishedIndexCheck = true;
        return acquisitionSW.getStatus();
    }

    const auto collAcquisition = acquisitionSW.getValue()->collection();

    const CollectionPtr& collection = collAcquisition.getCollectionPtr();

    auto indexSW = _acquireIndex(opCtx, collection, indexName);

    if (!indexSW.isOK()) {
        batchStats.finishedIndexBatch = true;
        batchStats.finishedIndexCheck = true;
        return indexSW.getStatus();
    }

    auto index = indexSW.getValue();
    // TODO (SERVER-83074): Enable special indexes in dbcheck.
    if (index->getAccessMethodName() != IndexNames::BTREE &&
        index->getAccessMethodName() != IndexNames::HASHED) {
        LOGV2_DEBUG(8033901,
                    3,
                    "Skip checking unsupported index.",
                    "collection"_attr = _info.nss,
                    "uuid"_attr = _info.uuid,
                    "indexName"_attr = index->indexName());

        status = Status(ErrorCodes::IndexOptionsConflict,
                        str::stream() << "index type is not supported, indexName: " << indexName
                                      << " for ns " << _info.nss.toStringForErrorMsg()
                                      << " and uuid " << _info.uuid.toString());

        batchStats.finishedIndexBatch = true;
        batchStats.finishedIndexCheck = true;
        return status;
    }


    // Set the index spec and keyPattern in batchStats for use in logging later.
    batchStats.keyPattern = index->keyPattern();
    batchStats.indexSpec = index->infoObj();

    // TODO SERVER-79846: Add testing for progress meter
    // {
    //     const std::string curOpMessage = "Scanning index " + indexName +
    //         " for namespace " + NamespaceStringUtil::serialize(info.nss);
    //     stdx::unique_lock<Client> lk(*opCtx->getClient());
    //     progress.set(lk,
    //                  CurOp::get(opCtx)->setProgress_inlock(
    //                      StringData(curOpMessage), collection->numRecords(opCtx)),
    //                  opCtx);
    // }

    // Set up index cursor.
    const IndexCatalogEntry* indexCatalogEntry =
        collection.get()->getIndexCatalog()->getEntry(index);
    const auto iam = indexCatalogEntry->accessMethod()->asSortedData();
    const auto ordering = iam->getSortedDataInterface()->getOrdering();
    const key_string::Version version = iam->getSortedDataInterface()->getKeyStringVersion();

    auto indexCursor =
        std::make_unique<SortedDataInterfaceThrottleCursor>(opCtx, iam, &_info.dataThrottle);

    // Set the index cursor's end position based on the inputted end parameter for when to stop
    // the dbcheck command.
    auto indexCursorEndKey = Helpers::toKeyFormat(_info.end);
    indexCursor->setEndPosition(indexCursorEndKey, true /*inclusive*/);
    int64_t numKeysInSnapshot = 0;
    int64_t numBytesInSnapshot = 0;


    BSONObj snapshotFirstKeyStringBsonRehydrated = BSONObj();
    boost::optional<KeyStringEntry> currIndexKeyWithRecordId = boost::none;

    // If we're in the middle of an index check, snapshotFirstKeyWithRecordId should be set.
    // Strip the recordId and seek.
    if (snapshotFirstKeyWithRecordId) {
        // If this is the beginning of a batch, update the batch first key.
        if (batchStats.batchStartWithRecordId.isEmpty() &&
            batchStats.batchStartBsonWithoutRecordId.isEmpty()) {
            _updateBatchStartForBatchStats(&batchStats, snapshotFirstKeyWithRecordId.get(), iam);
        }
        // Create keystring to seek without recordId. This is because if the index
        // is an older format unique index, the keystring will not have the recordId appended, so we
        // need to seek for the keystring without the recordId.
        auto snapshotFirstKeyWithoutRecordId =
            snapshotFirstKeyWithRecordId->getViewWithoutRecordId();
        snapshotFirstKeyStringBsonRehydrated = key_string::rehydrateKey(
            index->keyPattern(),
            _keyStringToBsonSafeHelper(snapshotFirstKeyWithRecordId.get(), ordering));

        // Seek for snapshotFirstKeyWithoutRecordId.
        // Note that seekForKeyString always returns a keyString with RecordId appended, regardless
        // of what format the index has.
        currIndexKeyWithRecordId =
            indexCursor->seekForKeyString(opCtx, snapshotFirstKeyWithoutRecordId);
    } else {
        // Set first key to _info.start. This is either $minKey or the user-specified start.
        if (batchStats.batchStartWithRecordId.isEmpty() &&
            batchStats.batchStartBsonWithoutRecordId.isEmpty()) {
            _updateBatchStartForBatchStats(&batchStats, _info.start, iam);
        }
        key_string::Builder keyStringBuilder(version);
        keyStringBuilder.resetToKey(_info.start, ordering);

        auto snapshotFirstKeyWithoutRecordId = keyStringBuilder.finishAndGetBuffer();

        // Seek for snapshotFirstKeyWithoutRecordId.
        // Note that seekForKeyString always returns a keyString with RecordId appended,
        // regardless of what format the index has.
        currIndexKeyWithRecordId =
            indexCursor->seekForKeyString(opCtx, snapshotFirstKeyWithoutRecordId);

        if (currIndexKeyWithRecordId) {
            snapshotFirstKeyStringBsonRehydrated = key_string::rehydrateKey(
                index->keyPattern(),
                _keyStringToBsonSafeHelper(currIndexKeyWithRecordId->keyString, ordering));
        }
    }

    // Note that if we can't find snapshotFirstKey (e.g. it was deleted in between snapshots),
    // seekForKeyString will automatically return the next adjacent keystring in the storage
    // engine. It will only return a null entry if there are no entries at all in the index.
    // Log for debug/testing purposes.
    if (!currIndexKeyWithRecordId) {
        LOGV2_DEBUG(7844803,
                    3,
                    "could not find any keys in index",
                    "endPosition"_attr =
                        key_string::rehydrateKey(index->keyPattern(), indexCursorEndKey),
                    "snapshotFirstKeyStringBson"_attr = snapshotFirstKeyStringBsonRehydrated,
                    "indexName"_attr = indexName,
                    logAttrs(_info.nss),
                    "uuid"_attr = _info.uuid);

        // Set batch end to be the user specified end or $maxKey.
        _updateBatchEndForBatchStats(&batchStats, _info.end, iam);
        batchStats.finishedIndexBatch = true;
        batchStats.finishedIndexCheck = true;
        return status;
    }

    LOGV2_DEBUG(7844800,
                3,
                "starting extra index keys snapshot at",
                "snapshotFirstKeyStringBson"_attr = snapshotFirstKeyStringBsonRehydrated,
                "indexName"_attr = indexName,
                logAttrs(_info.nss),
                "uuid"_attr = _info.uuid);

    // Loop until we should finish a snapshot.
    bool finishSnapshot = false;
    while (!finishSnapshot) {
        status = opCtx->checkForInterruptNoAssert();
        if (!status.isOK()) {
            return status;
        }

        const auto currKeyStringWithRecordId = currIndexKeyWithRecordId.get().keyString;
        const BSONObj currKeyStringBson =
            _keyStringToBsonSafeHelper(currKeyStringWithRecordId, ordering);

        if (!_info.secondaryIndexCheckParameters.get().getSkipLookupForExtraKeys()) {
            _reverseLookup(opCtx,
                           indexName,
                           batchStats,
                           collection,
                           currIndexKeyWithRecordId.get(),
                           currKeyStringBson,
                           index,
                           iam,
                           indexCatalogEntry,
                           index->infoObj());
        } else {
            LOGV2_DEBUG(7971700, 3, "Skipping reverse lookup for extra index keys dbcheck");
        }

        // Keep track of lastKey in batch.
        _updateLastKeyCheckedForBatchStats(&batchStats, currKeyStringWithRecordId, iam);
        _updateBatchEndForBatchStats(&batchStats, currKeyStringWithRecordId, iam);

        numBytesInSnapshot += currKeyStringWithRecordId.getSize();
        numKeysInSnapshot++;
        batchStats.nBytes += currKeyStringWithRecordId.getSize();
        batchStats.nKeys++;

        // Get the next keystring by advancing the cursor.
        // Note that nextKeyString always returns a keyString with RecordId appended, regardless
        // of what format the index has.
        auto nextIndexKeyWithRecordId = indexCursor->nextKeyString(opCtx);

        // Check if we should end the current catalog snapshot and/or batch and/or index check.
        // If there are more snapshots/batches left in the index check, updates batchStats with the
        // next snapshot's starting key.
        finishSnapshot = _shouldEndCatalogSnapshotOrBatch(opCtx,
                                                          collection,
                                                          indexName,
                                                          currKeyStringWithRecordId,
                                                          currKeyStringBson,
                                                          numKeysInSnapshot,
                                                          iam,
                                                          indexCursor,
                                                          batchStats,
                                                          nextIndexKeyWithRecordId);

        // Set our curr to the next keystring to be checked.
        currIndexKeyWithRecordId = nextIndexKeyWithRecordId;
    }

    LOGV2_DEBUG(7844808,
                3,
                "Catalog snapshot for reverse lookup check ending",
                "numKeys"_attr = numKeysInSnapshot,
                "numBytes"_attr = numBytesInSnapshot,
                "nConsecutiveIdenticalKeysAtEnd"_attr = batchStats.nConsecutiveIdenticalKeysAtEnd,
                "finishedIndexCheck"_attr = batchStats.finishedIndexCheck,
                "finishedIndexBatch"_attr = batchStats.finishedIndexBatch,
                logAttrs(_info.nss),
                "uuid"_attr = _info.uuid);
    return status;
}

/**
 * Returns if we should end the current catalog snapshot based on meeting snapshot/batch limits.
 * Also updates batchStats accordingly with the next batch's starting key, and whether
 * the batch and/or index check has finished.
 */
bool DbChecker::_shouldEndCatalogSnapshotOrBatch(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    StringData indexName,
    const key_string::Value& currKeyStringWithRecordId,
    const BSONObj& currKeyStringBson,
    const int64_t numKeysInSnapshot,
    const SortedDataIndexAccessMethod* iam,
    const std::unique_ptr<SortedDataInterfaceThrottleCursor>& indexCursor,
    DbCheckExtraIndexKeysBatchStats& batchStats,
    const boost::optional<KeyStringEntry>& nextIndexKeyWithRecordId) {

    // Helper for checking if there are still keys left in the index.
    auto checkNoMoreKeysInIndex =
        [&](const boost::optional<KeyStringEntry>& nextIndexKeyWithRecordId) {
            if (!nextIndexKeyWithRecordId) {
                batchStats.finishedIndexCheck = true;
                batchStats.finishedIndexBatch = true;
                _updateBatchEndForBatchStats(&batchStats, _info.end, iam);
                LOGV2_DEBUG(
                    7980004,
                    3,
                    "Finish the batch and the index check because there are no keys left in index");
                return true;
            }
            return false;
        };

    // If there are no more keys left in index, end the snapshot/batch/check.
    if (checkNoMoreKeysInIndex(nextIndexKeyWithRecordId)) {
        return true;
    }

    const IndexDescriptor* indexDescriptor =
        collection.get()->getIndexCatalog()->findIndexByName(opCtx, indexName);
    const auto ordering = iam->getSortedDataInterface()->getOrdering();
    const key_string::Version version = iam->getSortedDataInterface()->getKeyStringVersion();

    // Otherwise, set nextKeyToBeCheckedWithRecordId.
    batchStats.nextKeyToBeCheckedWithRecordId = nextIndexKeyWithRecordId.get().keyString;

    LOGV2_DEBUG(
        7980000,
        3,
        "comparing current keystring to next keystring",
        "curr"_attr = currKeyStringBson,
        "next"_attr = key_string::rehydrateKey(
            indexDescriptor->keyPattern(),
            _keyStringToBsonSafeHelper(nextIndexKeyWithRecordId.get().keyString, ordering)));

    const bool isDistinctNextKeyString = currKeyStringWithRecordId.compareWithoutRecordId(
                                             batchStats.nextKeyToBeCheckedWithRecordId,
                                             collection->getRecordStore()->keyFormat()) != 0;

    const bool shouldEndSnapshot =
        numKeysInSnapshot >= repl::dbCheckMaxTotalIndexKeysPerSnapshot.load();
    const bool shouldEndBatch =
        batchStats.nKeys >= _info.maxDocsPerBatch || Date_t::now() > batchStats.deadline;

    // If the next key is the same value as this one, we must look at them in the same
    // snapshot/batch, so skip this check.
    if (isDistinctNextKeyString) {
        // Check if we should finish this batch.
        if (shouldEndBatch) {
            LOGV2_DEBUG(8520200,
                        3,
                        "Finish the current batch because maxDocsPerBatch is met or the batch "
                        "deadline is met.",
                        "maxDocsPerBatch"_attr = _info.maxDocsPerBatch,
                        "batchStats.nKeys"_attr = batchStats.nKeys,
                        "batch deadline"_attr = batchStats.deadline);
            batchStats.finishedIndexBatch = true;
            return true;
        }

        // Check if we should finish this snapshot.
        if (shouldEndSnapshot) {
            LOGV2_DEBUG(7980001,
                        3,
                        "Finish the snapshot because dbCheckMaxTotalIndexKeysPerSnapshot "
                        "was reached");
            return true;
        }

        // Continue with same snapshot/batch.
        // Since the next key is a distinct key, and we are continuing in the same snapshot/batch,
        // reset nConsecutiveIdenticalKeysAtEnd.
        batchStats.nConsecutiveIdenticalKeysAtEnd = 1;
        return false;
    }

    // Consecutive Identical Key case.
    // If we've reached one of the other limits AND reached the max consecutive
    // identical index per snapshot limit, finish the batch.
    if ((shouldEndSnapshot || shouldEndBatch) &&
        batchStats.nConsecutiveIdenticalKeysAtEnd >=
            repl::dbCheckMaxConsecutiveIdenticalIndexKeysPerSnapshot.load()) {
        LOGV2_DEBUG(7980002,
                    3,
                    "Finish the current batch because the max consecutive identical "
                    "index keys per snapshot limit is met",
                    "nConsecutiveIdenticalKeysAtEnd"_attr =
                        batchStats.nConsecutiveIdenticalKeysAtEnd);

        batchStats.finishedIndexBatch = true;

        // We are ending this batch and the next batch should start at the next distinct key in the
        // index. Since batchStats.nextKeyToBeCheckedWithRecordId == currKeyStringWithRecordId (the
        // key we just checked), we need update it to the next distinct one. We make a keystring to
        // search with kExclusiveAfter so that seekForKeyString will seek to the next distinct
        // keyString after the current one.
        boost::optional<KeyStringEntry> maybeNextKeyToBeCheckedWithRecordId =
            _getNextDistinctKeyInIndex(opCtx, indexCursor, version, ordering, currKeyStringBson);
        // Check to make sure there are still more distinct keys in the index.
        // If there are no more keys left in index, finish the batch and the index check as a whole.
        bool noMoreKeysInIndex = checkNoMoreKeysInIndex(maybeNextKeyToBeCheckedWithRecordId);
        if (!noMoreKeysInIndex) {
            batchStats.nextKeyToBeCheckedWithRecordId =
                maybeNextKeyToBeCheckedWithRecordId.get().keyString;
        }
        return true;
    }

    // Otherwise, increment to represent the next key we will check in the same snapshot/batch.
    batchStats.nConsecutiveIdenticalKeysAtEnd++;
    return false;
}

void DbChecker::_reverseLookup(OperationContext* opCtx,
                               StringData indexName,
                               DbCheckExtraIndexKeysBatchStats& batchStats,
                               const CollectionPtr& collection,
                               const KeyStringEntry& keyStringEntryWithRecordId,
                               const BSONObj& keyStringBson,
                               const IndexDescriptor* indexDescriptor,
                               const SortedDataIndexAccessMethod* iam,
                               const IndexCatalogEntry* indexCatalogEntry,
                               const BSONObj& indexSpec) {
    auto seekRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
        opCtx, collection->getRecordStore(), &_info.dataThrottle);

    // WiredTiger always returns a KeyStringEntry with recordID as part of the contract of the
    // indexCursor.
    const key_string::Value keyStringWithRecordId = keyStringEntryWithRecordId.keyString;
    RecordId recordId = keyStringEntryWithRecordId.loc;
    invariant(!recordId.isNull());
    BSONObj keyStringBsonRehydrated =
        key_string::rehydrateKey(indexDescriptor->keyPattern(), keyStringBson);


    // Check that the recordId exists in the record store.
    auto record = seekRecordStoreCursor->seekExact(opCtx, recordId);
    if (!record) {
        LOGV2_DEBUG(7844802,
                    3,
                    "reverse lookup failed to find record data",
                    "recordId"_attr = recordId.toStringHumanReadable(),
                    "keyString"_attr = keyStringBsonRehydrated,
                    "indexName"_attr = indexName,
                    logAttrs(_info.nss),
                    "uuid"_attr = _info.uuid);

        Status status =
            Status(ErrorCodes::NoSuchKey,
                   str::stream() << "cannot find document from recordId "
                                 << recordId.toStringHumanReadable() << " from index " << indexName
                                 << " for ns " << _info.nss.toStringForErrorMsg());
        BSONObjBuilder context;
        context.append("keyString", keyStringBsonRehydrated);
        context.append("recordId", recordId.toStringHumanReadable());
        context.append("indexSpec", indexSpec);

        // TODO SERVER-79301: Update scope enums for health log entries.
        auto logEntry =
            dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                       _info.nss,
                                       _info.uuid,
                                       "found extra index key entry without corresponding document",
                                       ScopeEnum::Index,
                                       OplogEntriesEnum::Batch,
                                       status,
                                       context.done());
        HealthLogInterface::get(opCtx)->log(*logEntry);
        return;
    }

    // Found record in record store.
    auto recordBson = record->data.toBson();

    LOGV2_DEBUG(7844801,
                3,
                "reverse lookup found record data",
                "recordData"_attr = recordBson,
                "recordId"_attr = recordId.toStringHumanReadable(),
                "expectedKeyString"_attr = keyStringBsonRehydrated,
                "indexName"_attr = indexName,
                logAttrs(_info.nss),
                "uuid"_attr = _info.uuid);

    // Generate the set of keys for the record data and check that it includes the
    // index key.
    // TODO SERVER-80278: Make sure wildcard/multikey indexes are handled correctly here.
    KeyStringSet foundKeys;
    KeyStringSet multikeyMetadataKeys;
    MultikeyPaths multikeyPaths;
    SharedBufferFragmentBuilder pool(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    // A potential inefficiency with getKeys is that it generates all of the index keys
    // for this record for this secondary index, which means that if this index is a
    // multikey index, it could potentially be inefficient to generate all of them and only
    // check that it includes one specific keystring.
    iam->getKeys(opCtx,
                 collection,
                 indexCatalogEntry,
                 pool,
                 recordBson,
                 InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                 SortedDataIndexAccessMethod::GetKeysContext::kValidatingKeys,
                 &foundKeys,
                 &multikeyMetadataKeys,
                 &multikeyPaths,
                 recordId);

    if (foundKeys.contains(keyStringWithRecordId)) {
        return;
    }

    LOGV2_DEBUG(7844809,
                3,
                "found index key entry with corresponding document/keystring set that "
                "does not contain expected keystring",
                "recordData"_attr = recordBson,
                "recordId"_attr = recordId.toStringHumanReadable(),
                "expectedKeyString"_attr = keyStringBsonRehydrated,
                "indexName"_attr = indexName,
                logAttrs(_info.nss),
                "uuid"_attr = _info.uuid);
    Status status =
        Status(ErrorCodes::NoSuchKey,
               str::stream() << "found index key entry with corresponding document and "
                                "key string set that does not contain expected keystring "
                             << keyStringBsonRehydrated << " from index " << indexName << " for ns "
                             << _info.nss.toStringForErrorMsg());
    BSONObjBuilder context;
    context.append("expectedKeyString", keyStringBsonRehydrated);
    context.append("recordId", recordId.toStringHumanReadable());
    context.append("recordData", recordBson);
    context.append("indexSpec", indexSpec);

    // TODO SERVER-79301: Update scope enums for health log entries.
    auto logEntry = dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                               _info.nss,
                                               _info.uuid,
                                               "found index key entry with corresponding "
                                               "document/keystring set that does not "
                                               "contain the expected key string",
                                               ScopeEnum::Index,
                                               OplogEntriesEnum::Batch,
                                               status,
                                               context.done());
    HealthLogInterface::get(opCtx)->log(*logEntry);
    return;
}

bool DbChecker::_shouldRetryDataConsistencyCheck(OperationContext* opCtx,
                                                 Status status,
                                                 int numRetries) const {
    uassert(7985006,
            "status should never be OK if we are debating retrying data consistency check",
            !status.isOK());

    bool isRetryableError = false;
    auto logError = false;
    auto msg = "";

    const auto code = status.code();
    if (code == ErrorCodes::LockTimeout) {
        // This is a retryable error.
        isRetryableError = true;
        msg = "abandoning dbCheck batch after timeout due to lock unavailability";
    } else if (code == ErrorCodes::SnapshotUnavailable) {
        // This is a retryable error.
        isRetryableError = true;
        msg = "abandoning dbCheck batch after conflict with pending catalog operation";
    } else if (code == ErrorCodes::NamespaceNotFound) {
        msg = "abandoning dbCheck batch because collection no longer exists";
    } else if (code == ErrorCodes::CommandNotSupportedOnView) {
        msg =
            "abandoning dbCheck batch because collection no longer exists, but there "
            "is a "
            "view with the identical name";
    } else if (code == ErrorCodes::IndexNotFound) {
        msg = "skipping dbCheck on collection because it is missing an _id index";
        logError = true;
    } else if (code == ErrorCodes::ObjectIsBusy) {
        // This is a retryable error.
        isRetryableError = true;
        msg = "stopping dbCheck because a resource is in use by another process";
    } else if (code == ErrorCodes::NoSuchKey) {
        // We failed to parse or find an index key. Log a dbCheck error health log
        // entry.
        msg = "dbCheck found record with missing and/or mismatched index keys";
        logError = true;
    } else if (ErrorCodes::isA<ErrorCategory::NotPrimaryError>(code)) {
        msg = "abandoning dbCheck batch due to stepdown";
    } else {
        msg = "dbCheck failed with unexpected result";
        logError = true;
    }

    if (numRetries > repl::dbCheckMaxInternalRetries.load() || !isRetryableError) {
        std::unique_ptr<HealthLogEntry> entry;
        if (logError) {
            entry = dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                               _info.nss,
                                               _info.uuid,
                                               msg,
                                               ScopeEnum::Collection,
                                               OplogEntriesEnum::Batch,
                                               status);
        } else {
            entry = dbCheckWarningHealthLogEntry(_info.secondaryIndexCheckParameters,
                                                 _info.nss,
                                                 _info.uuid,
                                                 msg,
                                                 ScopeEnum::Collection,
                                                 OplogEntriesEnum::Batch,
                                                 status);
        }
        HealthLogInterface::get(opCtx)->log(*entry);
        return false;
    }
    return true;
}

// The initial amount of time to sleep between retries.
const int64_t initialSleepMillis = 100;

void DbChecker::_dataConsistencyCheck(OperationContext* opCtx) {
    const std::string curOpMessage = "Scanning namespace " +
        NamespaceStringUtil::serialize(_info.nss, SerializationContext::stateDefault());
    int64_t numRetries = 0;
    int64_t sleepMillis = initialSleepMillis;

    ProgressMeterHolder progress;
    bool retryProgressInitialization = true;
    while (retryProgressInitialization) {
        // Attempt to initialize progress meter.
        const auto acquisitionSW = _acquireDBCheckLocks(opCtx, _info.nss);
        if (!acquisitionSW.isOK()) {
            if (_shouldRetryDataConsistencyCheck(opCtx, acquisitionSW.getStatus(), numRetries)) {
                // The error is retryable. Sleep with increasing backoff.
                opCtx->sleepFor(Milliseconds(sleepMillis));
                numRetries++;
                sleepMillis *= 2;
                continue;
            }

            // Do not retry and return immediately.
            return;
        }

        // Set up progress tracker.
        const CollectionAcquisition collAcquisition = acquisitionSW.getValue()->collection();
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(
            lk,
            CurOp::get(opCtx)->setProgress(lk,
                                           StringData(curOpMessage),
                                           collAcquisition.getCollectionPtr()->numRecords(opCtx)),
            opCtx);
        retryProgressInitialization = false;
    }

    if (MONGO_unlikely(hangBeforeProcessingFirstBatch.shouldFail())) {
        LOGV2(7949001, "Hanging dbcheck due to failpoint 'hangBeforeProcessingFirstBatch'");
        hangBeforeProcessingFirstBatch.pauseWhileSet();
    }

    // Parameters for the hasher.
    auto start = _info.start;
    bool reachedEnd = false;

    // Make sure the totals over all of our batches don't exceed the provided limits.
    int64_t totalBytesSeen = 0;
    int64_t totalDocsSeen = 0;

    // Reset tracking values.
    numRetries = 0;
    sleepMillis = initialSleepMillis;

    do {
        auto result = _runBatch(opCtx, start);
        if (!result.isOK()) {
            if (!_shouldRetryDataConsistencyCheck(opCtx, result.getStatus(), numRetries)) {
                // We should not retry. Return immediately
                return;
            }

            // The error is retryable.. Sleep with increasing backoff.
            opCtx->sleepFor(Milliseconds(sleepMillis));
            sleepMillis *= 2;
            numRetries++;

            LOGV2_DEBUG(8365300,
                        3,
                        "Retrying dbCheck internally",
                        "numRetries"_attr = numRetries,
                        "status"_attr = result.getStatus());
            continue;
        }

        const auto stats = result.getValue();
        auto batchEnd = key_string::rehydrateKey(BSON("_id" << 1), stats.lastKey);
        auto entry = dbCheckBatchHealthLogEntry(_info.secondaryIndexCheckParameters,
                                                stats.batchId,
                                                _info.nss,
                                                _info.uuid,
                                                stats.nCount,
                                                stats.nBytes,
                                                stats.md5,
                                                stats.md5,
                                                key_string::rehydrateKey(BSON("_id" << 1), start),
                                                batchEnd,
                                                batchEnd /*lastKeyChecked*/,
                                                0 /*nConsecutiveIdenticalIndexKeysAtEnd*/,
                                                stats.readTimestamp,
                                                stats.time);
        if (kDebugBuild || entry->getSeverity() != SeverityEnum::Info || stats.logToHealthLog) {
            // On debug builds, health-log every batch result; on release builds, health-log
            // every N batches.
            HealthLogInterface::get(opCtx)->log(*entry);
        }

        WriteConcernResult unused;
        auto status = waitForWriteConcern(opCtx, stats.time, _info.writeConcern, &unused);
        if (!status.isOK()) {
            BSONObjBuilder context;
            if (stats.batchId) {
                context.append("batchId", stats.batchId->toBSON());
            }
            context.append("lastKey", stats.lastKey);
            auto entry = dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                                    _info.nss,
                                                    _info.uuid,
                                                    "dbCheck failed waiting for writeConcern",
                                                    ScopeEnum::Collection,
                                                    OplogEntriesEnum::Batch,
                                                    status,
                                                    context.done());
            HealthLogInterface::get(opCtx)->log(*entry);
            return;
        }

        start = stats.lastKey;

        // Update our running totals.
        totalDocsSeen += stats.nCount;
        totalBytesSeen += stats.nBytes;
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            progress.get(lk)->hit(stats.nDocs);
        }

        // Check if we've exceeded any limits.
        bool reachedLast = SimpleBSONObjComparator::kInstance.evaluate(stats.lastKey >= _info.end);
        bool tooManyDocs = totalDocsSeen >= _info.maxCount;
        bool tooManyBytes = totalBytesSeen >= _info.maxSize;
        reachedEnd = reachedLast || tooManyDocs || tooManyBytes;

        // Reset number of retries attempted so far.
        numRetries = 0;
        sleepMillis = initialSleepMillis;
    } while (!reachedEnd);

    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.get(lk)->finished();
    }
}

StatusWith<DbCheckCollectionBatchStats> DbChecker::_runBatch(OperationContext* opCtx,
                                                             const BSONObj& first) {
    DbCheckCollectionBatchStats result;
    DbCheckOplogBatch batch;
    {
        // We need to release the acquisition by dbcheck before writing to the oplog. This is
        // because dbcheck acquires the global lock in IS mode, and the oplog will attempt to
        // acquire the global lock in IX mode. Since we don't allow upgrading the global lock,
        // releasing the lock before writing to the oplog is essential.
        const auto acquisitionSW = _acquireDBCheckLocks(opCtx, _info.nss);
        if (!acquisitionSW.isOK()) {
            return acquisitionSW.getStatus();
        }

        // The CollectionPtr needs to outlive the DbCheckHasher as it's used internally.
        const CollectionPtr& collectionPtr =
            acquisitionSW.getValue()->collection().getCollectionPtr();
        if (collectionPtr.get()->uuid() != _info.uuid) {
            const auto msg = "Collection under dbCheck no longer exists";
            return {ErrorCodes::NamespaceNotFound, msg};
        }

        auto readTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
        uassert(ErrorCodes::SnapshotUnavailable,
                "No snapshot available yet for dbCheck",
                readTimestamp);

        boost::optional<DbCheckHasher> hasher;
        Status status = Status::OK();
        try {
            hasher.emplace(opCtx,
                           *acquisitionSW.getValue(),
                           first,
                           _info.end,
                           _info.secondaryIndexCheckParameters,
                           &_info.dataThrottle,
                           boost::none,
                           std::min(_info.maxDocsPerBatch, _info.maxCount),
                           _info.maxSize);

            const auto batchDeadline = Date_t::now() + Milliseconds(_info.maxBatchTimeMillis);
            status = hasher->hashForCollectionCheck(opCtx, collectionPtr, batchDeadline);
        } catch (const DBException& e) {
            return e.toStatus();
        }

        // ErrorCodes::DbCheckSecondaryBatchTimeout should only be thrown by the hasher on the
        // secondary.
        invariant(status.code() != ErrorCodes::DbCheckSecondaryBatchTimeout);
        if (!status.isOK()) {
            // dbCheck should still continue if we get an error fetching a record.
            if (status.code() == ErrorCodes::NoSuchKey) {
                std::unique_ptr<HealthLogEntry> healthLogEntry =
                    dbCheckErrorHealthLogEntry(_info.secondaryIndexCheckParameters,
                                               _info.nss,
                                               _info.uuid,
                                               "Error fetching record from record id",
                                               ScopeEnum::Index,
                                               OplogEntriesEnum::Batch,
                                               status);
                HealthLogInterface::get(opCtx)->log(*healthLogEntry);
            } else {
                return status;
            }
        }

        std::string md5 = hasher->total();

        batch.setType(OplogEntriesEnum::Batch);
        batch.setNss(_info.nss);
        batch.setMd5(md5);
        batch.setReadTimestamp(*readTimestamp);
        // TODO SERVER-78399: Remove special handling for BSONKey once feature flag is removed.
        if (_info.secondaryIndexCheckParameters) {
            batch.setSecondaryIndexCheckParameters(_info.secondaryIndexCheckParameters);

            // Set batchStart/batchEnd only if feature flag is on
            // (info.secondaryIndexCheckParameters is only boost::none if the feature flag is
            // off).
            batch.setBatchStart(first);
            batch.setBatchEnd(hasher->lastKeySeen());
        } else {
            // Otherwise set minKey/maxKey in BSONKey format.
            batch.setMinKey(BSONKey::parseFromBSON(first.firstElement()));
            batch.setMaxKey(BSONKey::parseFromBSON(hasher->lastKeySeen().firstElement()));
        }

        if (MONGO_unlikely(hangBeforeDbCheckLogOp.shouldFail())) {
            LOGV2(8230500, "Hanging dbcheck due to failpoint 'hangBeforeDbCheckLogOp'");
            hangBeforeDbCheckLogOp.pauseWhileSet();
        }

        auto [shouldLogBatch, batchId] = _shouldLogOplogBatch(batch);
        result.logToHealthLog = shouldLogBatch;
        result.batchId = batchId;
        result.readTimestamp = readTimestamp;
        result.nDocs = hasher->docsSeen();
        result.nCount = hasher->countSeen();
        result.nBytes = hasher->bytesSeen();
        result.lastKey = hasher->lastKeySeen();
        result.md5 = md5;
    }

    if (MONGO_unlikely(hangBeforeAddingDBCheckBatchToOplog.shouldFail())) {
        LOGV2(8589000, "Hanging dbCheck due to failpoint 'hangBeforeAddingDBCheckBatchToOplog'");
        hangBeforeAddingDBCheckBatchToOplog.pauseWhileSet();
    }

    // Send information on this batch over the oplog.
    auto opTimeSW = _logOp(
        opCtx, _info.nss, boost::none /* tenantIdForStartStop */, _info.uuid, batch.toBSON());
    if (!opTimeSW.isOK()) {
        return opTimeSW.getStatus();
    }
    result.time = opTimeSW.getValue();
    return result;
}

StatusWith<std::unique_ptr<DbCheckAcquisition>> DbChecker::_acquireDBCheckLocks(
    OperationContext* opCtx, const NamespaceString& nss) {
    // Each batch will read at the latest no-overlap point, which is the all_durable
    // timestamp on primaries. We assume that the history window on secondaries is always
    // longer than the time it takes between starting and replicating a batch on the
    // primary. Otherwise, the readTimestamp will not be available on a secondary by the
    // time it processes the oplog entry.
    auto readSource = ReadSourceWithTimestamp{RecoveryUnit::ReadSource::kNoOverlap};

    std::unique_ptr<DbCheckAcquisition> acquisition;

    try {
        // Acquires locks and sets appropriate state on the RecoveryUnit.
        acquisition =
            std::make_unique<DbCheckAcquisition>(opCtx,
                                                 _info.nss,
                                                 readSource,
                                                 // On the primary we must always block on prepared
                                                 // updates to guarantee snapshot isolation.
                                                 PrepareConflictBehavior::kEnforce);
    } catch (const DBException& ex) {
        // 'DbCheckAcquisition' fails with 'CommandNotSupportedOnView' if the namespace is referring
        // to a view.
        return ex.toStatus();
    }

    if (!acquisition->collection().exists() ||
        acquisition->collection().getCollectionPtr().get()->uuid() != _info.uuid) {
        Status status = Status(
            ErrorCodes::NamespaceNotFound,
            str::stream()
                << "Collection under dbCheck no longer exists; cannot find collection for ns "
                << _info.nss.toStringForErrorMsg() << " and uuid " << _info.uuid.toString());
        return status;
    }

    // At this point, we can be certain that no stepdown will occur throughout the lifespan of the
    // 'acquisition' object. Therefore, we should regularly check for interruption to prevent the
    // stepdown thread from waiting unnecessarily.
    return std::move(acquisition);
}

StatusWith<const IndexDescriptor*> DbChecker::_acquireIndex(OperationContext* opCtx,
                                                            const CollectionPtr& collection,
                                                            StringData indexName) {
    if (indexName == IndexConstants::kIdIndexName && collection->isClustered()) {
        Status status = Status(ErrorCodes::DbCheckAttemptOnClusteredCollectionIdIndex,
                               str::stream() << "Clustered collection doesn't have an _id index.");
        return status;
    }

    const IndexDescriptor* index =
        collection.get()->getIndexCatalog()->findIndexByName(opCtx, indexName);

    if (!index) {
        auto status = Status(ErrorCodes::IndexNotFound,
                             str::stream() << "cannot find index " << indexName << " for ns "
                                           << _info.nss.toStringForErrorMsg() << " and uuid "
                                           << _info.uuid.toString());
        return status;
    }
    return index;
}

std::pair<bool, boost::optional<UUID>> DbChecker::_shouldLogOplogBatch(DbCheckOplogBatch& batch) {
    _batchesProcessed++;
    bool shouldLog = (_batchesProcessed % gDbCheckHealthLogEveryNBatches.load() == 0);
    // TODO(SERVER-78399): Remove the check and always set the parameters of the batch.
    // Check 'gSecondaryIndexChecksInDbCheck' feature flag is enabled.
    if (batch.getSecondaryIndexCheckParameters()) {
        auto uuid = UUID::gen();
        batch.setLogBatchToHealthLog(shouldLog);
        batch.setBatchId(uuid);
        return {shouldLog, uuid};
    }

    return {shouldLog, boost::none};
}

void DbChecker::_updateBatchStartForBatchStats(DbCheckExtraIndexKeysBatchStats* batchStats,
                                               const key_string::Value batchStartWithRecordId,
                                               const SortedDataIndexAccessMethod* iam) const {
    uassert(7985005,
            "batchStats.batchStartWithRecordId must be empty if we are setting it",
            batchStats->batchStartWithRecordId.isEmpty() &&
                batchStats->batchStartBsonWithoutRecordId.isEmpty());
    batchStats->batchStartWithRecordId = batchStartWithRecordId;

    auto ordering = iam->getSortedDataInterface()->getOrdering();
    auto firstBsonWithoutRecordId =
        _keyStringToBsonSafeHelper(batchStats->batchStartWithRecordId, ordering);
    batchStats->batchStartBsonWithoutRecordId = firstBsonWithoutRecordId;
}

void DbChecker::_updateBatchStartForBatchStats(DbCheckExtraIndexKeysBatchStats* batchStats,
                                               const BSONObj batchStartBsonWithoutRecordId,
                                               const SortedDataIndexAccessMethod* iam) const {
    uassert(8632300,
            "batchStats.batchStartWithRecordId must be empty if we are setting it",
            batchStats->batchStartWithRecordId.isEmpty() &&
                batchStats->batchStartBsonWithoutRecordId.isEmpty());
    batchStats->batchStartBsonWithoutRecordId = batchStartBsonWithoutRecordId;

    const auto ordering = iam->getSortedDataInterface()->getOrdering();
    const auto version = iam->getSortedDataInterface()->getKeyStringVersion();

    key_string::Builder keyStringBuilder(version);
    keyStringBuilder.resetToKey(batchStartBsonWithoutRecordId, ordering);

    batchStats->batchStartWithRecordId = keyStringBuilder.getValueCopy();
}

void DbChecker::_updateLastKeyCheckedForBatchStats(
    DbCheckExtraIndexKeysBatchStats* batchStats,
    const key_string::Value lastKeyCheckedWithRecordId,
    const SortedDataIndexAccessMethod* iam) const {
    batchStats->lastKeyCheckedWithRecordId = lastKeyCheckedWithRecordId;

    auto ordering = iam->getSortedDataInterface()->getOrdering();
    auto lastBsonWithoutRecordId =
        _keyStringToBsonSafeHelper(batchStats->lastKeyCheckedWithRecordId, ordering);
    batchStats->lastBsonCheckedWithoutRecordId = lastBsonWithoutRecordId;
}

void DbChecker::_updateLastKeyCheckedForBatchStats(DbCheckExtraIndexKeysBatchStats* batchStats,
                                                   const BSONObj lastBsonCheckedWithoutRecordId,
                                                   const SortedDataIndexAccessMethod* iam) const {
    batchStats->lastBsonCheckedWithoutRecordId = lastBsonCheckedWithoutRecordId;

    const auto ordering = iam->getSortedDataInterface()->getOrdering();
    const auto version = iam->getSortedDataInterface()->getKeyStringVersion();

    key_string::Builder keyStringBuilder(version);
    keyStringBuilder.resetToKey(lastBsonCheckedWithoutRecordId, ordering);

    batchStats->lastKeyCheckedWithRecordId = keyStringBuilder.getValueCopy();
}

void DbChecker::_updateBatchEndForBatchStats(DbCheckExtraIndexKeysBatchStats* batchStats,
                                             const key_string::Value batchEndWithRecordId,
                                             const SortedDataIndexAccessMethod* iam) const {
    batchStats->batchEndWithRecordId = batchEndWithRecordId;

    auto ordering = iam->getSortedDataInterface()->getOrdering();
    auto batchEndBsonWithoutRecordId =
        _keyStringToBsonSafeHelper(batchStats->batchEndWithRecordId, ordering);
    batchStats->batchEndBsonWithoutRecordId = batchEndBsonWithoutRecordId;
}

void DbChecker::_updateBatchEndForBatchStats(DbCheckExtraIndexKeysBatchStats* batchStats,
                                             const BSONObj batchEndBsonWithoutRecordId,
                                             const SortedDataIndexAccessMethod* iam) const {
    batchStats->batchEndBsonWithoutRecordId = batchEndBsonWithoutRecordId;

    const auto ordering = iam->getSortedDataInterface()->getOrdering();
    const auto version = iam->getSortedDataInterface()->getKeyStringVersion();

    key_string::Builder keyStringBuilder(version);
    keyStringBuilder.resetToKey(batchEndBsonWithoutRecordId, ordering);

    batchStats->batchEndWithRecordId = keyStringBuilder.getValueCopy();
}

void DbChecker::_appendContextForLoggingExtraKeysCheck(DbCheckExtraIndexKeysBatchStats* batchStats,
                                                       BSONObjBuilder* builder) const {
    if (batchStats->batchId) {
        builder->append("batchId", batchStats->batchId->toBSON());
    }
    if (!batchStats->indexSpec.isEmpty()) {
        builder->append("indexSpec", batchStats->indexSpec);
    }
    if (!batchStats->batchStartBsonWithoutRecordId.isEmpty()) {
        builder->append("firstKey", batchStats->batchStartBsonWithoutRecordId);
    }
    if (!batchStats->lastBsonCheckedWithoutRecordId.isEmpty()) {
        builder->append("lastKey", batchStats->lastBsonCheckedWithoutRecordId);
    }
}

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

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Validate replica set consistency.\n"
               "Invoke with { dbCheck: <collection name/uuid>,\n"
               "              minKey: <first key, exclusive>,\n"
               "              maxKey: <last key, inclusive>,\n"
               "              maxCount: <try to keep a batch within maxCount number of docs>,\n"
               "              maxSize: <try to keep a batch withing maxSize of docs (bytes)>,\n"
               "              maxDocsPerBatch: <max number of docs/batch>\n"
               "              maxBatchTimeMillis: <max time processing a batch in "
               "milliseconds>\n"
               "to check a collection.\n"
               "Invoke with {dbCheck: 1} to check all collections in the database.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        const bool isAuthorized =
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(
                    ResourcePattern::forAnyResource(dbName.tenantId()), ActionType::dbCheck);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto job = getRun(opCtx, dbName, cmdObj);
        (new DbCheckJob(opCtx->getService(), std::move(job)))->go();
        return true;
    }
};
MONGO_REGISTER_COMMAND(DbCheckCmd).forShard();

}  // namespace mongo
