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


#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/health_log_gen.h"
#include "mongo/db/catalog/health_log_interface.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/dbcheck_command.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/index_access_method.h"
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
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand
MONGO_FAIL_POINT_DEFINE(hangBeforeExtraIndexKeysCheck);
MONGO_FAIL_POINT_DEFINE(hangBeforeReverseLookupCatalogSnapshot);
MONGO_FAIL_POINT_DEFINE(hangAfterReverseLookupCatalogSnapshot);
MONGO_FAIL_POINT_DEFINE(hangBeforeExtraIndexKeysHashing);
MONGO_FAIL_POINT_DEFINE(sleepAfterExtraIndexKeysHashing);

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeDbCheckLogOp);
MONGO_FAIL_POINT_DEFINE(hangBeforeProcessingDbCheckRun);
MONGO_FAIL_POINT_DEFINE(hangBeforeProcessingFirstBatch);
MONGO_FAIL_POINT_DEFINE(hangBeforeAddingDBCheckBatchToOplog);

// The optional `tenantIdForStartStop` is used for dbCheckStart/dbCheckStop oplog entries so that
// the namespace is still the admin command namespace but the tenantId will be set using the
// namespace that dbcheck is running for.
// This will acquire the global lock in IX mode.
repl::OpTime _logOp(OperationContext* opCtx,
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
    AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
    return writeConflictRetry(
        opCtx, "dbCheck oplog entry", NamespaceString::kRsOplogNamespace, [&] {
            auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
            oplogEntry.setWallClockTime(clockSource->now());

            WriteUnitOfWork uow(opCtx);
            repl::OpTime result = repl::logOp(opCtx, &oplogEntry);
            uow.commit();
            return result;
        });
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
    curOp->setNS_inlock(info->nss);
    curOp->setOpDescription_inlock(info->toBSON());
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
        auto wcDefault = ReadWriteConcernDefaults::get(opCtx->getServiceContext())
                             .getDefault(opCtx)
                             .getDefaultWriteConcern();
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
    try {
        DbCheckOplogStartStop oplogEntry;
        const auto nss = NamespaceString::kAdminCommandNamespace;
        oplogEntry.setNss(nss);
        oplogEntry.setType(OplogEntriesEnum::Start);

        auto healthLogEntry = dbCheckHealthLogEntry(boost::none /*nss*/,
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
            healthLogEntry->setData(_info.value().secondaryIndexCheckParameters.value().toBSON());

            oplogEntry.setNss(_info.value().nss);
            healthLogEntry->setNss(_info.value().nss);

            oplogEntry.setUuid(_info.value().uuid);
            healthLogEntry->setCollectionUUID(_info.value().uuid);

            if (_info && _info.value().nss.tenantId()) {
                tenantId = _info.value().nss.tenantId();
            }
        }

        HealthLogInterface::get(_opCtx)->log(*healthLogEntry);
        _logOp(_opCtx, nss, tenantId, boost::none /*uuid*/, oplogEntry.toBSON());
    } catch (const DBException& ex) {
        LOGV2(6202200, "Could not log start event", "error"_attr = ex.toString());
    }
}

DbCheckStartAndStopLogger::~DbCheckStartAndStopLogger() {
    try {
        DbCheckOplogStartStop oplogEntry;
        const auto nss = NamespaceString::kAdminCommandNamespace;
        oplogEntry.setNss(nss);
        oplogEntry.setType(OplogEntriesEnum::Stop);

        auto healthLogEntry = dbCheckHealthLogEntry(boost::none /*nss*/,
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
            healthLogEntry->setData(_info.value().secondaryIndexCheckParameters.value().toBSON());

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
        _logOp(_opCtx, nss, tenantId, boost::none /*uuid*/, oplogEntry.toBSON());
    } catch (const DBException& ex) {
        LOGV2(6202201, "Could not log stop event", "error"_attr = ex.toString());
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
        AutoGetCollectionForRead agc(opCtx, nss);
        uassert(ErrorCodes::NamespaceNotFound,
                "Collection " + invocation.getColl() + " not found",
                agc.getCollection());
        uuid = agc->uuid();
    } catch (const DBException& ex) {
        // 'AutoGetCollectionForRead' fails with 'CommandNotSupportedOnView' if the namespace is
        // referring to a view.
        uassert(ErrorCodes::CommandNotSupportedOnView,
                invocation.getColl() + " is a view hence 'dbcheck' is not supported.",
                ex.code() != ErrorCodes::CommandNotSupportedOnView);
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
    if (toParse["dbCheck"].type() == BSONType::String) {
        return singleCollectionRun(
            opCtx,
            dbName,
            DbCheckSingleInvocation::parse(IDLParserContext("",
                                                            false /*apiStrict*/,
                                                            auth::ValidatedTenancyScope::get(opCtx),
                                                            dbName.tenantId(),
                                                            SerializationContext::stateDefault()),
                                           toParse));
    } else {
        // Otherwise, it's the database-wide form.
        return fullDatabaseRun(
            opCtx,
            dbName,
            DbCheckAllInvocation::parse(IDLParserContext("",
                                                         false /*apiStrict*/,
                                                         auth::ValidatedTenancyScope::get(opCtx),
                                                         dbName.tenantId(),
                                                         SerializationContext::stateDefault()),
                                        toParse));
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
    DbCheckStartAndStopLogger startStop(opCtx, info);
    _initializeCurOp(opCtx, info);
    ON_BLOCK_EXIT([opCtx] { CurOp::get(opCtx)->done(); });

    if (MONGO_unlikely(hangBeforeProcessingDbCheckRun.shouldFail())) {
        LOGV2(7949000, "Hanging dbcheck due to failpoint 'hangBeforeProcessingDbCheckRun'");
        hangBeforeProcessingDbCheckRun.pauseWhileSet();
    }

    for (const auto& coll : *_run) {
        DbChecker dbChecker(coll);

        try {
            dbChecker.doCollection(opCtx);
        } catch (const DBException& ex) {
            auto errCode = ex.code();
            std::unique_ptr<HealthLogEntry> logEntry;
            if (errCode == ErrorCodes::CommandNotSupportedOnView) {
                // acquireCollectionMaybeLockFree throws CommandNotSupportedOnView if the
                // coll was dropped and a view with the same name was created.
                logEntry = dbCheckWarningHealthLogEntry(
                    coll.nss,
                    coll.uuid,
                    "abandoning dbCheck batch because collection no longer exists, but "
                    "there is a view with the identical name",
                    ScopeEnum::Collection,
                    OplogEntriesEnum::Batch,
                    Status(ErrorCodes::NamespaceNotFound,
                           "Collection under dbCheck no longer exists, but there is a view "
                           "with the identical name"));
            } else if (ErrorCodes::isA<ErrorCategory::NotPrimaryError>(errCode)) {
                logEntry = dbCheckWarningHealthLogEntry(
                    coll.nss,
                    coll.uuid,
                    "abandoning dbCheck batch due to stepdown.",
                    ScopeEnum::Collection,
                    OplogEntriesEnum::Batch,
                    Status(ErrorCodes::PrimarySteppedDown, "dbCheck terminated due to stepdown"));
            } else {
                logEntry = dbCheckErrorHealthLogEntry(coll.nss,
                                                      coll.uuid,
                                                      "dbCheck failed",
                                                      ScopeEnum::Cluster,
                                                      OplogEntriesEnum::Batch,
                                                      ex.toStatus());
            }
            HealthLogInterface::get(opCtx)->log(*logEntry);
            return;
        }
    }
}

void DbChecker::doCollection(OperationContext* opCtx) {
    // TODO SERVER-78399: Clean up this check once feature flag is removed.
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
}


StringData DbChecker::_stripRecordIdFromKeyString(const key_string::Value& keyString,
                                                  const key_string::Version& version,
                                                  const Collection* collection) {
    const size_t keyStringSize = getKeyStringSizeWithoutRecordId(collection, keyString);
    return {keyString.getBuffer(), keyStringSize};
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
        // TODO SERVER-81592: Revisit case where skipLookupForExtraKeys is true, if we can
        // avoid doing two index walks (one for batching and one for hashing).
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
            // TODO SERVER-79850: Raise all errors to the upper level.
            if (reverseLookupStatus.code() != ErrorCodes::IndexNotFound) {
                // Raise the error to the upper level.
                iassert(reverseLookupStatus);
            }
            break;
        }

        // 2. Get the actual last keystring processed from reverse lookup.
        // If the first or last key of the batch is not initialized, that means there was an error
        // with batching.
        if (batchStats.firstKeyCheckedWithRecordId.isEmpty() ||
            batchStats.lastKeyCheckedWithRecordId.isEmpty()) {
            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
            // one location.
            LOGV2_DEBUG(7844903,
                        3,
                        "abandoning extra index keys check because of error with batching",
                        "indexName"_attr = indexName,
                        logAttrs(_info.nss),
                        "uuid"_attr = _info.uuid);
            Status status = Status(ErrorCodes::NoSuchKey,
                                   "could not create batch bounds because of error while batching");
            const auto logEntry = dbCheckErrorHealthLogEntry(
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
        if (MONGO_unlikely(sleepAfterExtraIndexKeysHashing.shouldFail())) {
            LOGV2_DEBUG(3083201,
                        3,
                        "Sleeping for 1 second due to sleepAfterExtraIndexKeysHashing failpoint");
            opCtx->sleepFor(Milliseconds(1000));
        }
        if (!hashStatus.isOK()) {
            LOGV2_DEBUG(7844902,
                        3,
                        "abandoning extra index keys check because of error with hashing",
                        "status"_attr = hashStatus.reason(),
                        "indexName"_attr = indexName,
                        logAttrs(_info.nss),
                        "uuid"_attr = _info.uuid);
            // TODO SERVER-79850: Raise all errors to the upper level.
            if (hashStatus.code() != ErrorCodes::IndexNotFound &&
                hashStatus.code() != ErrorCodes::WriteConcernFailed &&
                hashStatus.code() != ErrorCodes::UnsatisfiableWriteConcern) {
                // Raise the error to the upper level.
                iassert(hashStatus);
            }
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

    auto status = _runHashExtraKeyCheck(opCtx, batchStats);
    if (!status.isOK()) {
        return status;
    }

    auto logEntry =
        dbCheckBatchEntry(batchStats->batchId,
                          _info.nss,
                          _info.uuid,
                          batchStats->nHasherKeys,
                          batchStats->nHasherBytes,
                          batchStats->md5,
                          batchStats->md5,
                          key_string::rehydrateKey(batchStats->keyPattern, batchStats->firstBson),
                          key_string::rehydrateKey(batchStats->keyPattern, batchStats->lastBson),
                          batchStats->nConsecutiveIdenticalKeysAtEnd,
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
    status = waitForWriteConcern(opCtx, batchStats->time, _info.writeConcern, &unused);

    if (!status.isOK()) {
        BSONObjBuilder context;
        if (batchStats->batchId) {
            context.append("batchId", batchStats->batchId->toBSON());
        }
        context.append("firstKey", batchStats->firstBson);
        context.append("lastKey", batchStats->lastBson);

        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors
        // in one location.
        auto entry = dbCheckErrorHealthLogEntry(_info.nss,
                                                _info.uuid,
                                                "dbCheck failed waiting for writeConcern",
                                                ScopeEnum::Collection,
                                                OplogEntriesEnum::Batch,
                                                status,
                                                context.done());
        HealthLogInterface::get(opCtx)->log(*entry);
        return status;
    }
    return Status::OK();
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
        const auto acquisition = _acquireDBCheckLocks(opCtx, _info.nss);
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
        if (!acquisition->coll.exists() ||
            acquisition->coll.getCollectionPtr().get()->uuid() != _info.uuid) {
            Status status = Status(ErrorCodes::IndexNotFound,
                                   str::stream() << "cannot find collection for ns "
                                                 << _info.nss.toStringForErrorMsg() << " and uuid "
                                                 << _info.uuid.toString());
            const auto logEntry = dbCheckWarningHealthLogEntry(
                _info.nss,
                _info.uuid,
                "abandoning dbCheck extra index keys check because collection no longer exists",
                ScopeEnum::Index,
                OplogEntriesEnum::Batch,
                status);
            HealthLogInterface::get(opCtx)->log(*logEntry);
            batchStats->finishedIndexCheck = true;

            return status;
        }
        const CollectionPtr& collection = acquisition->coll.getCollectionPtr();

        auto readTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
        uassert(ErrorCodes::SnapshotUnavailable,
                "No snapshot available yet for dbCheck extra index keys check",
                readTimestamp);

        const IndexDescriptor* index =
            collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
        if (!index) {
            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
            // one location.
            Status status = Status(ErrorCodes::IndexNotFound,
                                   str::stream() << "cannot find index " << indexName << " for ns "
                                                 << _info.nss.toStringForErrorMsg() << " and uuid "
                                                 << _info.uuid.toString());
            const auto logEntry = dbCheckWarningHealthLogEntry(
                _info.nss,
                _info.uuid,
                "abandoning dbCheck extra index keys check because index no longer exists",
                ScopeEnum::Index,
                OplogEntriesEnum::Batch,
                status);
            HealthLogInterface::get(opCtx)->log(*logEntry);
            batchStats->finishedIndexCheck = true;
            return status;
        }

        const IndexCatalogEntry* indexCatalogEntry = collection->getIndexCatalog()->getEntry(index);
        auto iam = indexCatalogEntry->accessMethod()->asSortedData();
        const auto ordering = iam->getSortedDataInterface()->getOrdering();

        // Converting to BSON strips the recordId from the keystring.
        auto firstBsonWithoutRecordId =
            _keyStringToBsonSafeHelper(batchStats->firstKeyCheckedWithRecordId, ordering);
        auto lastBsonWithoutRecordId =
            _keyStringToBsonSafeHelper(batchStats->lastKeyCheckedWithRecordId, ordering);

        LOGV2_DEBUG(8520000,
                    3,
                    "Beginning hash for extra keys batch",
                    "firstKeyString"_attr = firstBsonWithoutRecordId,
                    "lastKeyString"_attr = lastBsonWithoutRecordId);

        // Create hasher.
        boost::optional<DbCheckHasher> hasher;
        try {
            hasher.emplace(opCtx,
                           *acquisition,
                           firstBsonWithoutRecordId,
                           lastBsonWithoutRecordId,
                           _info.secondaryIndexCheckParameters,
                           &_info.dataThrottle,
                           indexName,
                           std::min(_info.maxDocsPerBatch, _info.maxCount),
                           _info.maxSize);
        } catch (const DBException& e) {
            return e.toStatus();
        }

        Status status = hasher->hashForExtraIndexKeysCheck(
            opCtx, collection.get(), firstBsonWithoutRecordId, lastBsonWithoutRecordId);
        if (!status.isOK()) {
            return status;
        }

        // Send information on this batch over the oplog.
        std::string md5 = hasher->total();
        oplogBatch.setType(OplogEntriesEnum::Batch);
        oplogBatch.setNss(_info.nss);
        oplogBatch.setReadTimestamp(*readTimestamp);
        oplogBatch.setMd5(md5);
        oplogBatch.setBatchStart(firstBsonWithoutRecordId);
        oplogBatch.setBatchEnd(lastBsonWithoutRecordId);

        if (_info.secondaryIndexCheckParameters) {
            oplogBatch.setSecondaryIndexCheckParameters(_info.secondaryIndexCheckParameters);
        }

        LOGV2_DEBUG(7844900,
                    3,
                    "hashed one batch on primary",
                    "firstKeyString"_attr =
                        key_string::rehydrateKey(index->keyPattern(), firstBsonWithoutRecordId),
                    "lastKeyString"_attr =
                        key_string::rehydrateKey(index->keyPattern(), lastBsonWithoutRecordId),
                    "md5"_attr = md5,
                    "keysHashed"_attr = hasher->keysSeen(),
                    "bytesHashed"_attr = hasher->bytesSeen(),
                    "nConsecutiveIdenticalIndexKeysSeenAtEnd"_attr =
                        hasher->nConsecutiveIdenticalIndexKeysSeenAtEnd(),
                    "readTimestamp"_attr = readTimestamp,
                    "indexName"_attr = indexName,
                    logAttrs(_info.nss),
                    "uuid"_attr = _info.uuid);

        auto [shouldLogBatch, batchId] = _shouldLogBatch(oplogBatch);
        batchStats->logToHealthLog = shouldLogBatch;
        batchStats->batchId = batchId;
        batchStats->readTimestamp = readTimestamp;
        batchStats->nHasherKeys = hasher->keysSeen();
        batchStats->nHasherBytes = hasher->bytesSeen();
        batchStats->md5 = md5;
        batchStats->keyPattern = index->keyPattern();
        batchStats->indexSpec = index->infoObj();

        batchStats->firstBson = firstBsonWithoutRecordId;
        batchStats->lastBson = lastBsonWithoutRecordId;
    }

    if (MONGO_unlikely(hangBeforeAddingDBCheckBatchToOplog.shouldFail())) {
        LOGV2(8831800, "Hanging dbCheck due to failpoint 'hangBeforeAddingDBCheckBatchToOplog'");
        hangBeforeAddingDBCheckBatchToOplog.pauseWhileSet();
    }

    batchStats->time = _logOp(
        opCtx, _info.nss, boost::none /* tenantIdForStartStop */, _info.uuid, oplogBatch.toBSON());
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
            return status;
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
    const auto acquisition = _acquireDBCheckLocks(opCtx, _info.nss);
    const auto collAcquisition = acquisition->coll;
    if (!collAcquisition.exists() ||
        collAcquisition.getCollectionPtr().get()->uuid() != _info.uuid) {
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
        status = Status(ErrorCodes::IndexNotFound,
                        str::stream()
                            << "cannot find collection for ns " << _info.nss.toStringForErrorMsg()
                            << " and uuid " << _info.uuid.toString());
        const auto logEntry = dbCheckWarningHealthLogEntry(
            _info.nss,
            _info.uuid,
            "abandoning dbCheck extra index keys check because collection no longer exists",
            ScopeEnum::Collection,
            OplogEntriesEnum::Batch,
            status);
        HealthLogInterface::get(opCtx)->log(*logEntry);
        batchStats.finishedIndexBatch = true;
        batchStats.finishedIndexCheck = true;

        return status;
    }
    const CollectionPtr& collection = collAcquisition.getCollectionPtr();
    const IndexDescriptor* index =
        collection.get()->getIndexCatalog()->findIndexByName(opCtx, indexName);

    if (!index) {
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
        status = Status(ErrorCodes::IndexNotFound,
                        str::stream() << "cannot find index " << indexName << " for ns "
                                      << _info.nss.toStringForErrorMsg() << " and uuid "
                                      << _info.uuid.toString());
        const auto logEntry = dbCheckWarningHealthLogEntry(
            _info.nss,
            _info.uuid,
            "abandoning dbCheck extra index keys check because index no longer exists",
            ScopeEnum::Index,
            OplogEntriesEnum::Batch,
            status);
        HealthLogInterface::get(opCtx)->log(*logEntry);
        batchStats.finishedIndexBatch = true;
        batchStats.finishedIndexCheck = true;

        return status;
    }

    // TODO (SERVER-83074): Enable special indexes in dbcheck.
    if (index->getAccessMethodName() != IndexNames::BTREE &&
        index->getAccessMethodName() != IndexNames::HASHED) {
        LOGV2_DEBUG(8033901,
                    3,
                    "Skip checking unsupported index.",
                    "collection"_attr = _info.nss,
                    "uuid"_attr = _info.uuid,
                    "indexName"_attr = index->indexName());
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
        status = Status(ErrorCodes::IndexNotFound,
                        str::stream() << "index type is not supported, indexName: " << indexName
                                      << " for ns " << _info.nss.toStringForErrorMsg()
                                      << " and uuid " << _info.uuid.toString());
        const auto logEntry = dbCheckWarningHealthLogEntry(
            _info.nss,
            _info.uuid,
            "abandoning dbCheck extra index keys check because index type is not supported",
            ScopeEnum::Index,
            OplogEntriesEnum::Batch,
            status);
        HealthLogInterface::get(opCtx)->log(*logEntry);

        batchStats.finishedIndexBatch = true;
        batchStats.finishedIndexCheck = true;
        return status;
    }

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
    auto maxKey = Helpers::toKeyFormat(_info.end);
    indexCursor->setEndPosition(maxKey, true /*inclusive*/);
    int64_t numKeysInSnapshot = 0;
    int64_t numBytesInSnapshot = 0;


    BSONObj snapshotFirstKeyStringBsonRehydrated = BSONObj();
    boost::optional<KeyStringEntry> currIndexKeyWithRecordId = boost::none;

    // If we're in the middle of an index check, snapshotFirstKeyWithRecordId should be set.
    // Strip the recordId and seek.
    if (snapshotFirstKeyWithRecordId.is_initialized()) {
        // Create keystring to seek without recordId. This is because if the index
        // is an older format unique index, the keystring will not have the recordId appended, so we
        // need to seek for the keystring without the recordId.
        auto snapshotFirstKeyWithoutRecordId = _stripRecordIdFromKeyString(
            snapshotFirstKeyWithRecordId.get(), version, collection.get());
        snapshotFirstKeyStringBsonRehydrated = key_string::rehydrateKey(
            index->keyPattern(),
            _keyStringToBsonSafeHelper(snapshotFirstKeyWithRecordId.get(), ordering));

        // Seek for snapshotFirstKeyWithoutRecordId.
        // Note that seekForKeyString always returns a keyString with RecordId appended, regardless
        // of what format the index has.
        currIndexKeyWithRecordId =
            indexCursor->seekForKeyString(opCtx, snapshotFirstKeyWithoutRecordId);
    } else {
        // If we're at the beginning of the entire index check (snapshotFirstKeyWithRecordId is not
        // set), and the user has provided a start key, set snapshotFirstKeyWithoutRecordId to the
        // start key.
        if (SimpleBSONObjComparator::kInstance.evaluate(BSONObj::stripFieldNames(_info.start) !=
                                                        kMinBSONKey)) {
            key_string::Builder keyStringBuilder(version);
            keyStringBuilder.resetToKey(_info.start, ordering);

            auto snapshotFirstKeyWithoutRecordId = keyStringBuilder.finishAndGetBuffer();
            snapshotFirstKeyStringBsonRehydrated = key_string::rehydrateKey(
                index->keyPattern(), _builderToBsonSafeHelper(keyStringBuilder, ordering));

            // seek for snapshotFirstKeyWithoutRecordId.
            // Note that seekForKeyString always returns a keyString with RecordId appended,
            // regardless of what format the index has.
            currIndexKeyWithRecordId =
                indexCursor->seekForKeyString(opCtx, snapshotFirstKeyWithoutRecordId);
        } else {

            // Otherwise, we're at the beginning of the entire index check, and the user has not
            // provided a start key, so just call nextKeyString, which will return the first key in
            // the index.
            //
            // Note that nextKeyString always returns a keyString with RecordId appended, regardless
            // of what format the index has.
            currIndexKeyWithRecordId = indexCursor->nextKeyString(opCtx);

            // Set the snapshot first keystring to the first key in the index. If this does not
            // exist, we will log empty BSON and fail the dbCheck later.
            if (currIndexKeyWithRecordId) {
                snapshotFirstKeyStringBsonRehydrated = key_string::rehydrateKey(
                    index->keyPattern(),
                    _keyStringToBsonSafeHelper(currIndexKeyWithRecordId->keyString, ordering));
            }
        }
    }

    // Note that if we can't find snapshotFirstKey (e.g. it was deleted in between snapshots),
    // seekForKeyString will automatically return the next adjacent keystring in the storage
    // engine. It will only return a null entry if there are no entries at all in the index.
    // Log for debug/testing purposes.
    if (!currIndexKeyWithRecordId) {
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
        LOGV2_DEBUG(7844803,
                    3,
                    "could not find any keys in index",
                    "endPosition"_attr = maxKey,
                    "snapshotFirstKeyStringBson"_attr = snapshotFirstKeyStringBsonRehydrated,
                    "indexName"_attr = indexName,
                    logAttrs(_info.nss),
                    "uuid"_attr = _info.uuid);
        status = Status(ErrorCodes::IndexNotFound,
                        str::stream() << "cannot find any keys in index " << indexName << " for ns "
                                      << _info.nss.toStringForErrorMsg() << " and uuid "
                                      << _info.uuid.toString());
        BSONObjBuilder context;
        context.append("indexSpec", index->infoObj());
        const auto logEntry =
            dbCheckWarningHealthLogEntry(_info.nss,
                                         _info.uuid,
                                         "abandoning dbCheck extra index keys check because "
                                         "there are no keys left in the index",
                                         ScopeEnum::Index,
                                         OplogEntriesEnum::Batch,
                                         status,
                                         context.done());
        HealthLogInterface::get(opCtx)->log(*logEntry);
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

    // Keep track of first key checked in the batch if it hasn't been set already, so that we can
    // keep track of the batch bounds for hashing purposes.
    if (batchStats.firstKeyCheckedWithRecordId.isEmpty()) {
        batchStats.firstKeyCheckedWithRecordId = currIndexKeyWithRecordId.get().keyString;
    }

    // Loop until we should finish a snapshot.
    bool finishSnapshot = false;
    while (!finishSnapshot) {
        iassert(opCtx->checkForInterruptNoAssert());
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
                           iam,
                           indexCatalogEntry,
                           index->infoObj());
        } else {
            LOGV2_DEBUG(7971700, 3, "Skipping reverse lookup for extra index keys dbcheck");
        }

        // Keep track of lastKey in batch.
        batchStats.lastKeyCheckedWithRecordId = currKeyStringWithRecordId;

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
        [&batchStats](const boost::optional<KeyStringEntry>& nextIndexKeyWithRecordId) {
            if (!nextIndexKeyWithRecordId) {
                batchStats.finishedIndexCheck = true;
                batchStats.finishedIndexBatch = true;
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

    const bool isDistinctNextKeyString = [&] {
        switch (collection->getRecordStore()->keyFormat()) {
            case KeyFormat::Long:
                return currKeyStringWithRecordId.compareWithoutRecordIdLong(
                           batchStats.nextKeyToBeCheckedWithRecordId) != 0;

            case KeyFormat::String:
                return currKeyStringWithRecordId.compareWithoutRecordIdStr(
                           batchStats.nextKeyToBeCheckedWithRecordId) != 0;
        }
        MONGO_UNREACHABLE;
    }();

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
        key_string::Builder builder(version);
        auto keyStringForSeekWithoutRecordId =
            IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                currKeyStringBson, ordering, true /*isForward*/, false /*inclusive*/, builder);


        // Check to make sure there are still more distinct keys in the index.
        boost::optional<KeyStringEntry> maybeNextKeyToBeCheckedWithRecordId =
            indexCursor->seekForKeyString(opCtx, keyStringForSeekWithoutRecordId);
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
                               const SortedDataIndexAccessMethod* iam,
                               const IndexCatalogEntry* indexCatalogEntry,
                               const BSONObj& indexSpec) {
    auto seekRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
        opCtx, collection->getRecordStore(), &_info.dataThrottle);

    const IndexDescriptor* indexDescriptor =
        collection.get()->getIndexCatalog()->findIndexByName(opCtx, indexName);

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
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
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
            dbCheckErrorHealthLogEntry(_info.nss,
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
    auto logEntry = dbCheckErrorHealthLogEntry(_info.nss,
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

// The initial amount of time to sleep between retries.
const int64_t initialSleepMillis = 100;

void DbChecker::_dataConsistencyCheck(OperationContext* opCtx) {
    const std::string curOpMessage = "Scanning namespace " +
        NamespaceStringUtil::serialize(_info.nss, SerializationContext::stateDefault());
    ProgressMeterHolder progress;
    {
        bool collectionFound = false;
        std::string collNotFoundMsg = "Collection under dbCheck no longer exists";
        try {
            const CollectionAcquisition collAcquisition = acquireCollectionMaybeLockFree(
                opCtx,
                CollectionAcquisitionRequest::fromOpCtx(
                    opCtx, _info.nss, AcquisitionPrerequisites::OperationType::kRead));
            if (collAcquisition.exists() &&
                collAcquisition.getCollectionPtr().get()->uuid() == _info.uuid) {
                stdx::unique_lock<Client> lk(*opCtx->getClient());
                progress.set(lk,
                             CurOp::get(opCtx)->setProgress_inlock(
                                 StringData(curOpMessage),
                                 collAcquisition.getCollectionPtr()->numRecords(opCtx)),
                             opCtx);
                collectionFound = true;
            }
        } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
            // 'acquireCollectionMaybeLockFree' fails with 'CommandNotSupportedOnView' if
            // the namespace is referring to a view. This case can happen if the collection
            // got dropped and then a view got created with the same name before calling
            // 'acquireCollectionMaybeLockFree'.
            // Don't throw and instead log a health entry.
            collNotFoundMsg += ", but there is a view with the identical name";
        } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>&) {
            // 'acquireCollectionMaybeLockFree' fails with CollectionUUIDMismatch if the
            // collection/view we found with nss has an uuid that does not match info.uuid.
            // Don't throw and instead log a health entry.
        }

        if (!collectionFound) {
            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors
            // in one location.
            const auto entry = dbCheckWarningHealthLogEntry(
                _info.nss,
                _info.uuid,
                "abandoning dbCheck batch because collection no longer exists",
                ScopeEnum::Collection,
                OplogEntriesEnum::Batch,
                Status(ErrorCodes::NamespaceNotFound, collNotFoundMsg));
            HealthLogInterface::get(opCtx)->log(*entry);
            return;
        }
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

    int64_t numRetries = 0;
    int64_t sleepMillis = initialSleepMillis;

    do {
        auto result = _runBatch(opCtx, start);
        if (!result.isOK()) {
            auto retryable = false;
            auto logError = false;
            auto msg = "";

            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors
            // in one location.
            const auto code = result.getStatus().code();
            if (code == ErrorCodes::LockTimeout) {
                // This is a retryable error.
                retryable = true;
                msg = "abandoning dbCheck batch after timeout due to lock unavailability";
            } else if (code == ErrorCodes::SnapshotUnavailable) {
                // This is a retryable error.
                retryable = true;
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
            } else if (code == ErrorCodes::ObjectIsBusy) {
                // This is a retryable error.
                retryable = true;
                msg = "stopping dbCheck because a resource is in use by another process";
            } else if (code == ErrorCodes::NoSuchKey) {
                // We failed to parse or find an index key. Log a dbCheck error health log
                // entry.
                msg = "dbCheck found record with missing and/or mismatched index keys";
                logError = true;
            } else {
                // Raise the error to the upper level.
                iassert(result);
            }

            if (retryable && numRetries++ < repl::dbCheckMaxInternalRetries.load()) {
                // Retryable error. Sleep with increasing backoff.
                opCtx->sleepFor(Milliseconds(sleepMillis));
                sleepMillis *= 2;

                LOGV2_DEBUG(8365300,
                            3,
                            "Retrying dbCheck internally",
                            "numRetries"_attr = numRetries,
                            "status"_attr = result.getStatus());
                continue;
            }

            // Cannot retry. Write a health log entry and return from the batch.
            std::unique_ptr<HealthLogEntry> entry;
            if (logError) {
                entry = dbCheckErrorHealthLogEntry(_info.nss,
                                                   _info.uuid,
                                                   msg,
                                                   ScopeEnum::Collection,
                                                   OplogEntriesEnum::Batch,
                                                   result.getStatus());
            } else {
                entry = dbCheckWarningHealthLogEntry(_info.nss,
                                                     _info.uuid,
                                                     msg,
                                                     ScopeEnum::Collection,
                                                     OplogEntriesEnum::Batch,
                                                     result.getStatus());
            }
            HealthLogInterface::get(opCtx)->log(*entry);
            return;
        }

        const auto stats = result.getValue();

        auto entry = dbCheckBatchEntry(stats.batchId,
                                       _info.nss,
                                       _info.uuid,
                                       stats.nCount,
                                       stats.nBytes,
                                       stats.md5,
                                       stats.md5,
                                       key_string::rehydrateKey(BSON("_id" << 1), start),
                                       key_string::rehydrateKey(BSON("_id" << 1), stats.lastKey),
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
            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors
            // in one location.
            auto entry = dbCheckErrorHealthLogEntry(_info.nss,
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
        const auto acquisition = _acquireDBCheckLocks(opCtx, _info.nss);
        if (!acquisition->coll.exists()) {
            const auto msg = "Collection under dbCheck no longer exists";
            return {ErrorCodes::NamespaceNotFound, msg};
        }
        // The CollectionPtr needs to outlive the DbCheckHasher as it's used internally.
        const CollectionPtr& collectionPtr = acquisition->coll.getCollectionPtr();
        if (collectionPtr.get()->uuid() != _info.uuid) {
            const auto msg = "Collection under dbCheck no longer exists";
            return {ErrorCodes::NamespaceNotFound, msg};
        }

        auto readTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
        uassert(ErrorCodes::SnapshotUnavailable,
                "No snapshot available yet for dbCheck",
                readTimestamp);

        boost::optional<DbCheckHasher> hasher;
        Status status = Status::OK();
        try {
            hasher.emplace(opCtx,
                           *acquisition,
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

        if (!status.isOK()) {
            // dbCheck should still continue if we get an error fetching a record.
            if (status.code() == ErrorCodes::NoSuchKey) {
                std::unique_ptr<HealthLogEntry> healthLogEntry =
                    dbCheckErrorHealthLogEntry(_info.nss,
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

        auto [shouldLogBatch, batchId] = _shouldLogBatch(batch);
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
    result.time = _logOp(
        opCtx, _info.nss, boost::none /* tenantIdForStartStop */, _info.uuid, batch.toBSON());
    return result;
}

std::unique_ptr<DbCheckAcquisition> DbChecker::_acquireDBCheckLocks(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    // Each batch will read at the latest no-overlap point, which is the all_durable
    // timestamp on primaries. We assume that the history window on secondaries is always
    // longer than the time it takes between starting and replicating a batch on the
    // primary. Otherwise, the readTimestamp will not be available on a secondary by the
    // time it processes the oplog entry.
    auto readSource = ReadSourceWithTimestamp{RecoveryUnit::ReadSource::kNoOverlap};

    // Acquires locks and sets appropriate state on the RecoveryUnit.
    auto acquisition = std::make_unique<DbCheckAcquisition>(
        opCtx,
        _info.nss,
        readSource,
        // On the primary we must always block on prepared updates to guarantee snapshot isolation.
        PrepareConflictBehavior::kEnforce);
    // At this point, we can be certain that no stepdown will occur throughout the lifespan of the
    // 'acquisition' object. Therefore, we should regularly check for interruption to prevent the
    // stepdown thread from waiting unnecessarily.
    return acquisition;
}

std::pair<bool, boost::optional<UUID>> DbChecker::_shouldLogBatch(DbCheckOplogBatch& batch) {
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
