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
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
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

boost::optional<key_string::Value> DbChecker::getExtraIndexKeysCheckLookupStart(
    OperationContext* opCtx) {
    StringData indexName = _info.secondaryIndexCheckParameters.get().getSecondaryIndex();
    const auto acquisition = _acquireDBCheckLocks(opCtx, _info.nss);
    const auto collAcquisition = acquisition->coll;
    if (!collAcquisition.exists() ||
        collAcquisition.getCollectionPtr().get()->uuid() != _info.uuid) {
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
        Status status = Status(ErrorCodes::IndexNotFound,
                               str::stream() << "cannot find collection for ns "
                                             << _info.nss.toStringForErrorMsg() << " and uuid "
                                             << _info.uuid.toString());
        const auto logEntry = dbCheckWarningHealthLogEntry(
            _info.nss,
            _info.uuid,
            "abandoning dbCheck extra index keys check because collection no longer exists",
            ScopeEnum::Collection,
            OplogEntriesEnum::Batch,
            status);
        HealthLogInterface::get(opCtx)->log(*logEntry);
        return boost::none;
    }
    const CollectionPtr& collection = collAcquisition.getCollectionPtr();
    const IndexDescriptor* index =
        collection.get()->getIndexCatalog()->findIndexByName(opCtx, indexName);

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
        return boost::none;
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
        return boost::none;
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

    const IndexCatalogEntry* indexCatalogEntry =
        collection.get()->getIndexCatalog()->getEntry(index);
    auto iam = indexCatalogEntry->accessMethod()->asSortedData();
    const auto ordering = iam->getSortedDataInterface()->getOrdering();
    const key_string::Version version = iam->getSortedDataInterface()->getKeyStringVersion();

    if (SimpleBSONObjComparator::kInstance.evaluate(BSONObj::stripFieldNames(_info.start) ==
                                                    kMinBSONKey)) {
        auto indexCursor =
            std::make_unique<SortedDataInterfaceThrottleCursor>(opCtx, iam, &_info.dataThrottle);
        boost::optional<KeyStringEntry> firstKeyStringEntry = indexCursor->nextKeyString(opCtx);
        boost::optional<key_string::Value> firstKeyString;
        if (firstKeyStringEntry) {
            firstKeyString = firstKeyStringEntry.get().keyString;
        }
        return firstKeyString;
    } else {
        key_string::Builder firstKeyString(version);
        firstKeyString.resetToKey(_info.start, ordering);
        return firstKeyString.getValueCopy();
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

    // Get catalog snapshot to look up the firstKey in the index.
    boost::optional<key_string::Value> maybeLookupStart = getExtraIndexKeysCheckLookupStart(opCtx);
    // If no first key was returned that means the index was not found, and we should exit the
    // dbCheck.
    if (!maybeLookupStart) {
        return;
    }
    key_string::Value lookupStart = maybeLookupStart.get();

    bool reachedEnd = false;

    int64_t totalBytesSeen = 0;
    int64_t totalKeysSeen = 0;
    do {
        DbCheckExtraIndexKeysBatchStats batchStats = {0};
        batchStats.deadline = Date_t::now() + Milliseconds(_info.maxBatchTimeMillis);

        // 1. Get batch bounds (stored in batchStats) and run reverse lookup if
        // skipLookupForExtraKeys is not set.
        // TODO SERVER-81592: Revisit case where skipLookupForExtraKeys is true, if we can
        // avoid doing two index walks (one for batching and one for hashing).
        auto batchFirst = lookupStart;
        Status reverseLookupStatus =
            _getExtraIndexKeysBatchAndRunReverseLookup(opCtx, indexName, lookupStart, batchStats);
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

        // 2. Get the actual first and last keystrings processed from reverse lookup.
        batchFirst = batchStats.firstIndexKey;
        auto batchLast = batchStats.lastIndexKey;

        // If batchLast is not initialized, that means there was an error with batching.
        if (batchLast.isEmpty()) {
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
        Status hashStatus = _hashExtraIndexKeysCheck(opCtx, batchFirst, batchLast, &batchStats);
        if (!hashStatus.isOK()) {
            LOGV2_DEBUG(7844902,
                        3,
                        "abandoning extra index keys check because of error with hashing",
                        "status"_attr = hashStatus.reason(),
                        "indexName"_attr = indexName,
                        logAttrs(_info.nss),
                        "uuid"_attr = _info.uuid);
            // TODO SERVER-79850: Raise all errors to the upper level.
            if (hashStatus.code() != ErrorCodes::IndexNotFound) {
                // Raise the error to the upper level.
                iassert(hashStatus);
            }
            break;
        }


        // 4. Update lookupStart to resume the next batch.
        lookupStart = batchStats.nextLookupStart;

        // TODO SERVER-79846: Add testing for progress meter
        // {
        //     stdx::unique_lock<Client> lk(*opCtx->getClient());
        //     progress.get(lk)->hit(batchStats.nDocs);
        // }

        // 5. Check if we've exceeded any limits.
        _batchesProcessed++;
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
                                           const key_string::Value& batchFirst,
                                           const key_string::Value& batchLast,
                                           DbCheckExtraIndexKeysBatchStats* batchStats) {
    if (MONGO_unlikely(hangBeforeExtraIndexKeysHashing.shouldFail())) {
        LOGV2_DEBUG(7844906, 3, "Hanging due to hangBeforeExtraIndexKeysHashing failpoint");
        hangBeforeExtraIndexKeysHashing.pauseWhileSet(opCtx);
    }
    StringData indexName = _info.secondaryIndexCheckParameters.get().getSecondaryIndex();
    DbCheckOplogBatch oplogBatch;
    BSONObjBuilder builder;
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
            batchStats->finishedIndexBatch = true;
            batchStats->finishedIndexCheck = true;

            return status;
        }
        const CollectionPtr& collection = acquisition->coll.getCollectionPtr();

        auto readTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
        uassert(ErrorCodes::SnapshotUnavailable,
                "No snapshot available yet for dbCheck extra index keys check",
                readTimestamp);
        batchStats->readTimestamp = readTimestamp;


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
            batchStats->finishedIndexBatch = true;
            batchStats->finishedIndexCheck = true;
            return status;
        }
        const IndexCatalogEntry* indexCatalogEntry = collection->getIndexCatalog()->getEntry(index);
        auto iam = indexCatalogEntry->accessMethod()->asSortedData();
        const auto ordering = iam->getSortedDataInterface()->getOrdering();
        auto firstBson = key_string::toBsonSafe(
            batchFirst.getBuffer(), batchFirst.getSize(), ordering, batchFirst.getTypeBits());
        auto lastBson = key_string::toBsonSafe(
            batchLast.getBuffer(), batchLast.getSize(), ordering, batchLast.getTypeBits());

        // Create hasher.
        boost::optional<DbCheckHasher> hasher;
        try {
            hasher.emplace(opCtx,
                           *acquisition,
                           firstBson,
                           lastBson,
                           _info.secondaryIndexCheckParameters,
                           &_info.dataThrottle,
                           indexName,
                           std::min(_info.maxDocsPerBatch, _info.maxCount),
                           _info.maxSize);
        } catch (const DBException& e) {
            return e.toStatus();
        }

        Status status =
            hasher->hashForExtraIndexKeysCheck(opCtx, collection.get(), batchFirst, batchLast);
        if (!status.isOK()) {
            return status;
        }


        // Send information on this batch over the oplog.
        std::string md5 = hasher->total();
        batchStats->md5 = md5;
        oplogBatch.setType(OplogEntriesEnum::Batch);
        oplogBatch.setNss(_info.nss);
        oplogBatch.setReadTimestamp(*readTimestamp);
        oplogBatch.setMd5(md5);
        oplogBatch.setBatchStart(firstBson);
        oplogBatch.setBatchEnd(lastBson);

        if (_info.secondaryIndexCheckParameters) {
            oplogBatch.setSecondaryIndexCheckParameters(_info.secondaryIndexCheckParameters);
        }

        LOGV2_DEBUG(7844900,
                    3,
                    "hashed one batch on primary",
                    "firstKeyString"_attr =
                        key_string::rehydrateKey(index->keyPattern(), firstBson),
                    "lastKeyString"_attr = key_string::rehydrateKey(index->keyPattern(), lastBson),
                    "md5"_attr = md5,
                    "keysHashed"_attr = hasher->keysSeen(),
                    "bytesHashed"_attr = hasher->bytesSeen(),
                    "readTimestamp"_attr = readTimestamp,
                    "indexName"_attr = indexName,
                    logAttrs(_info.nss),
                    "uuid"_attr = _info.uuid);

        builder.append("success", true);
        builder.append("count", hasher->keysSeen());
        builder.append("bytes", hasher->bytesSeen());
        builder.append("md5", batchStats->md5);
        builder.append("minKey", firstBson);
        builder.append("maxKey", lastBson);
        if (readTimestamp) {
            builder.append("readTimestamp", *readTimestamp);
        }
    }

    batchStats->time = _logOp(
        opCtx, _info.nss, boost::none /* tenantIdForStartStop */, _info.uuid, oplogBatch.toBSON());

    builder.append("optime", batchStats->time.toBSON());
    auto logEntry = dbCheckHealthLogEntry(_info.nss,
                                          _info.uuid,
                                          SeverityEnum::Info,
                                          "dbcheck extra keys check batch on primary",
                                          ScopeEnum::Index,
                                          OplogEntriesEnum::Batch,
                                          builder.obj());

    if (kDebugBuild || logEntry->getSeverity() != SeverityEnum::Info ||
        (_batchesProcessed % gDbCheckHealthLogEveryNBatches.load() == 0)) {
        // On debug builds, health-log every batch result; on release builds, health-log
        // every N batches.
        HealthLogInterface::get(opCtx)->log(*logEntry);
    }
    return Status::OK();
}


/**
 * Gets batch bounds for extra index keys check and stores the info in batchStats. Runs
 * reverse lookup if skipLookupForExtraKeys is not set.
 * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
 */
Status DbChecker::_getExtraIndexKeysBatchAndRunReverseLookup(
    OperationContext* opCtx,
    const StringData& indexName,
    key_string::Value& lookupStart,
    DbCheckExtraIndexKeysBatchStats& batchStats) {
    bool reachedBatchEnd = false;
    do {
        auto status =
            _getCatalogSnapshotAndRunReverseLookup(opCtx, indexName, lookupStart, batchStats);
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
        lookupStart = batchStats.nextLookupStart;
    } while (!reachedBatchEnd && !batchStats.finishedIndexCheck);
    return Status::OK();
}

/**
 * Acquires a consistent catalog snapshot and iterates through the secondary index in order
 * to get the batch bounds. Runs reverse lookup if skipLookupForExtraKeys is not set.
 *
 * We release the snapshot by exiting the function. This occurs when we've either finished
 * the whole extra index keys check, finished one batch, or the number of keys we've looked
 * at has met or exceeded dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot.
 *
 * Returns a non-OK Status if we encountered an error and should abandon extra index keys check.
 */
Status DbChecker::_getCatalogSnapshotAndRunReverseLookup(
    OperationContext* opCtx,
    const StringData& indexName,
    const key_string::Value& lookupStart,
    DbCheckExtraIndexKeysBatchStats& batchStats) {
    if (MONGO_unlikely(hangBeforeReverseLookupCatalogSnapshot.shouldFail())) {
        LOGV2_DEBUG(7844804, 3, "Hanging due to hangBeforeReverseLookupCatalogSnapshot failpoint");
        hangBeforeReverseLookupCatalogSnapshot.pauseWhileSet(opCtx);
    }

    Status status = Status::OK();
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

    const IndexCatalogEntry* indexCatalogEntry =
        collection.get()->getIndexCatalog()->getEntry(index);
    auto iam = indexCatalogEntry->accessMethod()->asSortedData();
    const auto ordering = iam->getSortedDataInterface()->getOrdering();


    auto indexCursor =
        std::make_unique<SortedDataInterfaceThrottleCursor>(opCtx, iam, &_info.dataThrottle);


    // Set the index cursor's end position based on the inputted end parameter for when to stop
    // the dbcheck command.
    auto maxKey = Helpers::toKeyFormat(_info.end);
    indexCursor->setEndPosition(maxKey, true /*inclusive*/);
    int64_t numKeys = 0;
    int64_t numBytes = 0;

    auto lookupStartKeyStringBsonRehydrated = key_string::rehydrateKey(
        index->keyPattern(),
        key_string::toBsonSafe(
            lookupStart.getBuffer(), lookupStart.getSize(), ordering, lookupStart.getTypeBits()));

    LOGV2_DEBUG(7844800,
                3,
                "starting extra index keys batch at",
                "lookupStartKeyStringBson"_attr = lookupStartKeyStringBsonRehydrated,
                "indexName"_attr = indexName,
                logAttrs(_info.nss),
                "uuid"_attr = _info.uuid);

    auto currIndexKey = indexCursor->seekForKeyString(opCtx, lookupStart);

    // Note that if we can't find lookupStart (e.g. it was deleted in between snapshots),
    // seekForKeyString will automatically return the next adjacent keystring in the storage
    // engine. It will only return a null entry if there are no entries at all in the index.
    // Log for debug/testing purposes.
    if (!currIndexKey) {
        // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
        // one location.
        LOGV2_DEBUG(7844803,
                    3,
                    "could not find any keys in index",
                    "endPosition"_attr = maxKey,
                    "lookupStartKeyStringBson"_attr = lookupStartKeyStringBsonRehydrated,
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

    // Track actual first key in batch, since it might not be the same as lookupStart if the
    // index keys have changed between reverse lookup catalog snapshots.
    const auto firstKeyString = currIndexKey.get().keyString;
    batchStats.firstIndexKey = firstKeyString;

    while (currIndexKey) {
        iassert(opCtx->checkForInterruptNoAssert());
        const auto keyString = currIndexKey.get().keyString;
        const BSONObj keyStringBson = key_string::toBsonSafe(
            keyString.getBuffer(), keyString.getSize(), ordering, keyString.getTypeBits());

        if (!_info.secondaryIndexCheckParameters.get().getSkipLookupForExtraKeys()) {
            _reverseLookup(opCtx,
                           indexName,
                           batchStats,
                           collection,
                           keyString,
                           keyStringBson,
                           iam,
                           indexCatalogEntry,
                           index->infoObj());
        } else {
            LOGV2_DEBUG(7971700, 3, "Skipping reverse lookup for extra index keys dbcheck");
        }

        batchStats.lastIndexKey = keyString;
        numBytes += keyString.getSize();
        numKeys++;
        batchStats.nBytes += keyString.getSize();
        batchStats.nKeys++;

        currIndexKey = indexCursor->nextKeyString(opCtx);

        // Set nextLookupStart.
        if (currIndexKey) {
            batchStats.nextLookupStart = currIndexKey.get().keyString;
        }

        // TODO SERVER-79800: Fix handling of identical index keys.
        // If the next key is the same value as this one, we must look at them in the same
        // snapshot/batch, so skip this check.
        if (!(currIndexKey && (keyString == currIndexKey.get().keyString))) {
            // Check if we should finish this batch.
            if (batchStats.nKeys >= _info.maxDocsPerBatch) {
                LOGV2_DEBUG(8520200,
                            3,
                            "Finish the current batch because maxDocsPerBatch is met.",
                            "maxDocsPerBatch"_attr = _info.maxDocsPerBatch,
                            "batchStats.nKeys"_attr = batchStats.nKeys);
                batchStats.finishedIndexBatch = true;
                break;
            }
            // Check if we should release snapshot.
            if (numKeys >= repl::dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot.load()) {
                break;
            }
        }

        if (Date_t::now() > batchStats.deadline) {
            LOGV2_DEBUG(8520201,
                        3,
                        "Finish the current batch because batch deadline is met.",
                        "batch deadline"_attr = batchStats.deadline);
            batchStats.finishedIndexBatch = true;
            break;
        }
    }


    batchStats.finishedIndexCheck = !currIndexKey.is_initialized();
    LOGV2_DEBUG(7844808,
                3,
                "Catalog snapshot for extra index keys check ending",
                "numKeys"_attr = numKeys,
                "numBytes"_attr = numBytes,
                "finishedIndexCheck"_attr = batchStats.finishedIndexCheck,
                "finishedIndexBatch"_attr = batchStats.finishedIndexBatch,
                logAttrs(_info.nss),
                "uuid"_attr = _info.uuid);
    return status;
}


void DbChecker::_reverseLookup(OperationContext* opCtx,
                               const StringData& indexName,
                               DbCheckExtraIndexKeysBatchStats& batchStats,
                               const CollectionPtr& collection,
                               const key_string::Value& keyString,
                               const BSONObj& keyStringBson,
                               const SortedDataIndexAccessMethod* iam,
                               const IndexCatalogEntry* indexCatalogEntry,
                               const BSONObj& indexSpec) {
    // Check that the recordId exists in the record store.
    // TODO SERVER-80654: Handle secondary indexes with the old format that doesn't store
    // keystrings with the RecordId appended.
    auto recordId = [&] {
        switch (collection->getRecordStore()->keyFormat()) {
            case KeyFormat::Long:
                return key_string::decodeRecordIdLongAtEnd(keyString.getBuffer(),
                                                           keyString.getSize());
            case KeyFormat::String:
                return key_string::decodeRecordIdStrAtEnd(keyString.getBuffer(),
                                                          keyString.getSize());
        }
        MONGO_UNREACHABLE;
    }();

    auto seekRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
        opCtx, collection->getRecordStore(), &_info.dataThrottle);

    const IndexDescriptor* indexDescriptor =
        collection.get()->getIndexCatalog()->findIndexByName(opCtx, indexName);
    auto record = seekRecordStoreCursor->seekExact(opCtx, recordId);
    BSONObj keyStringBsonRehydrated =
        key_string::rehydrateKey(indexDescriptor->keyPattern(), keyStringBson);
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
    // TODO SERVER-80654: Handle secondary indexes with the old format that doesn't store
    // keystrings with the RecordId appended.
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

    LOGV2_DEBUG(7844801,
                3,
                "reverse lookup found record data",
                "recordData"_attr = recordBson,
                "recordId"_attr = recordId.toStringHumanReadable(),
                "expectedKeyString"_attr = keyStringBsonRehydrated,
                "indexName"_attr = indexName,
                logAttrs(_info.nss),
                "uuid"_attr = _info.uuid);

    if (foundKeys.contains(keyString)) {
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
            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
            // one location.
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

            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
            // one location.
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
                    "abandoning dbCheck batch because collection no longer exists, but there is a "
                    "view with the identical name";
            } else if (code == ErrorCodes::IndexNotFound) {
                msg = "skipping dbCheck on collection because it is missing an _id index";
            } else if (code == ErrorCodes::ObjectIsBusy) {
                // This is a retryable error.
                retryable = true;
                msg = "stopping dbCheck because a resource is in use by another process";
            } else if (code == ErrorCodes::NoSuchKey) {
                // We failed to parse or find an index key. Log a dbCheck error health log entry.
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
                                       stats.nDocs,
                                       stats.nBytes,
                                       stats.md5,
                                       stats.md5,
                                       start,
                                       stats.lastKey,
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
            // TODO SERVER-79850: Investigate refactoring dbcheck code to only check for errors in
            // one location.
            auto entry = dbCheckWarningHealthLogEntry(_info.nss,
                                                      _info.uuid,
                                                      "dbCheck failed waiting for writeConcern",
                                                      ScopeEnum::Collection,
                                                      OplogEntriesEnum::Batch,
                                                      status);
            HealthLogInterface::get(opCtx)->log(*entry);
        }

        start = stats.lastKey;

        // Update our running totals.
        totalDocsSeen += stats.nDocs;
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
            batch.setBatchEnd(hasher->lastKey());
        } else {
            // Otherwise set minKey/maxKey in BSONKey format.
            batch.setMinKey(BSONKey::parseFromBSON(first.firstElement()));
            batch.setMaxKey(BSONKey::parseFromBSON(hasher->lastKey().firstElement()));
        }

        if (MONGO_unlikely(hangBeforeDbCheckLogOp.shouldFail())) {
            LOGV2(8230500, "Hanging dbcheck due to failpoint 'hangBeforeDbCheckLogOp'");
            hangBeforeDbCheckLogOp.pauseWhileSet();
        }

        result.logToHealthLog = _shouldLogBatch(batch);
        result.batchId = batch.getBatchId();
        result.readTimestamp = readTimestamp;
        result.nDocs = hasher->docsSeen();
        result.nBytes = hasher->bytesSeen();
        result.lastKey = hasher->lastKey();
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

bool DbChecker::_shouldLogBatch(DbCheckOplogBatch& batch) {
    _batchesProcessed++;
    bool shouldLog = (_batchesProcessed % gDbCheckHealthLogEveryNBatches.load() == 0);
    // TODO(SERVER-78399): Remove the check and always set the parameters of the batch.
    // Check 'gSecondaryIndexChecksInDbCheck' feature flag is enabled.
    if (batch.getSecondaryIndexCheckParameters()) {
        batch.setLogBatchToHealthLog(shouldLog);
        batch.setBatchId(UUID::gen());
    }

    return shouldLog;
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

    virtual bool run(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        auto job = getRun(opCtx, dbName, cmdObj);
        (new DbCheckJob(opCtx->getService(), std::move(job)))->go();
        return true;
    }
};
MONGO_REGISTER_COMMAND(DbCheckCmd).forShard();

}  // namespace mongo
