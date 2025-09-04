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

#include "mongo/db/repl/apply_ops.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace repl {

constexpr StringData ApplyOps::kOplogApplicationModeFieldName;

namespace {

// If enabled, causes loop in _applyOps() to hang after applying current operation.
MONGO_FAIL_POINT_DEFINE(applyOpsPauseBetweenOperations);

Status _applyOps(OperationContext* opCtx,
                 const ApplyOpsCommandInfo& info,
                 const DatabaseName& dbName,
                 repl::OplogApplication::Mode oplogApplicationMode,
                 BSONObjBuilder* result,
                 int* numApplied,
                 BSONArrayBuilder* opsBuilder) {
    const auto& ops = info.getOperations();
    // apply
    *numApplied = 0;
    int errors = 0;

    BSONArrayBuilder ab;
    // Apply each op in the given 'applyOps' command object.
    for (const auto& opObj : ops) {
        // Ignore 'n' operations.
        const char* opType = opObj.getStringField("op").data();
        if (*opType == 'n')
            continue;

        // opObj["ns"] contains a tenantId prefixed namespace if there is tenancy.
        const NamespaceString nss(NamespaceStringUtil::deserialize(
            dbName.tenantId(), opObj["ns"].String(), SerializationContext::stateDefault()));

        if (*opType != 'c' && !nss.isValid())
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.toStringForErrorMsg()};

        Status status = Status::OK();

        try {
            status = writeConflictRetryWithLimit(
                opCtx,
                "applyOps",
                nss,
                [opCtx, nss, opObj, opType, oplogApplicationMode, &info, &dbName] {
                    BSONObjBuilder builder;
                    builder.appendElements(opObj);
                    if (!builder.hasField(OplogEntry::kTimestampFieldName)) {
                        builder.append(OplogEntry::kTimestampFieldName, Timestamp());
                    }
                    if (!builder.hasField(OplogEntry::kWallClockTimeFieldName)) {
                        builder.append(OplogEntry::kWallClockTimeFieldName, Date_t());
                    }
                    auto entry = uassertStatusOK(OplogEntry::parse(builder.done()));

                    // VersionContext fixes a FCV snapshot over the opCtx, making FCV-gated feature
                    // flags checks in secondaries behave as they did on the primary, thus ensuring
                    // correct application even if the FCV changed due to a concurrent setFCV.
                    boost::optional<VersionContext::ScopedSetDecoration> scopedVersionContext;
                    if (entry.getVersionContext()) {
                        scopedVersionContext.emplace(opCtx, *entry.getVersionContext());
                    }

                    switch (entry.getOpType()) {
                        case OpTypeEnum::kContainerInsert:
                        case OpTypeEnum::kContainerDelete: {
                            if (const auto fcv =
                                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
                                !(fcv.isVersionInitialized() &&
                                  ::mongo::feature_flags::gFeatureFlagPrimaryDrivenIndexBuilds
                                      .isEnabled(VersionContext::getDecoration(opCtx), fcv)) &&
                                oplogApplicationMode == OplogApplication::Mode::kApplyOpsCmd) {
                                uasserted(ErrorCodes::InvalidOptions,
                                          "Container ops are not enabled");
                            }
                            auto coll = acquireCollection(opCtx,
                                                          {nss,
                                                           PlacementConcern::kPretendUnsharded,
                                                           ReadConcernArgs::get(opCtx),
                                                           AcquisitionPrerequisites::kWrite},
                                                          MODE_IX);
                            uassertStatusOK(applyContainerOperation_inlock(
                                opCtx, ApplierOperation{&entry}, oplogApplicationMode));
                            return Status::OK();
                        }
                        case OpTypeEnum::kCommand: {
                            if (entry.getCommandType() == OplogEntry::CommandType::kDropDatabase) {
                                invariant(info.getOperations().size() == 1,
                                          "dropDatabase in applyOps must be the only entry");
                                // This method is explicitly called without locks in spite of the
                                // _inlock suffix. dropDatabase cannot hold any locks for execution
                                // of the operation due to potential replication waits.
                                uassertStatusOK(applyCommand_inlock(
                                    opCtx, ApplierOperation{&entry}, oplogApplicationMode));
                                return Status::OK();
                            }
                            invariant(shard_role_details::getLocker(opCtx)->isW());
                            if (entry.getCommandType() == OplogEntry::CommandType::kCreate) {
                                // Allow apply ops for a create oplog entry to create the collection
                                // locally. This will bypass sharding, but we expect that users
                                // running applyOps know what they are doing and will handle this.
                                const auto& ns = OplogApplication::extractNsFromCmd(
                                    entry.getNss().dbName(), entry.getObject());
                                OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                                    allowCreate(opCtx, ns);
                                uassertStatusOK(applyCommand_inlock(
                                    opCtx, ApplierOperation{&entry}, oplogApplicationMode));
                                return Status::OK();
                            }
                            uassertStatusOK(applyCommand_inlock(
                                opCtx, ApplierOperation{&entry}, oplogApplicationMode));
                            return Status::OK();
                        }
                        case OpTypeEnum::kInsert:
                        case OpTypeEnum::kUpdate:
                        case OpTypeEnum::kDelete: {
                            // If the namespace and uuid passed into applyOps point to different
                            // namespaces, throw an error.
                            auto catalog = CollectionCatalog::get(opCtx);
                            if (opObj.hasField("ui")) {
                                auto uuid = UUID::parse(opObj["ui"]).getValue();
                                auto nssFromUuid = catalog->lookupNSSByUUID(opCtx, uuid);
                                if (nssFromUuid != nss) {
                                    return Status{ErrorCodes::Error(3318200),
                                                  str::stream()
                                                      << "Namespace '" << nss.toStringForErrorMsg()
                                                      << "' and UUID '" << uuid.toString()
                                                      << "' point to different collections"};
                                }
                            }

                            auto collection = acquireCollection(
                                opCtx,
                                CollectionAcquisitionRequest(nss,
                                                             PlacementConcern::kPretendUnsharded,
                                                             repl::ReadConcernArgs::get(opCtx),
                                                             AcquisitionPrerequisites::kWrite),
                                fixLockModeForSystemDotViewsChanges(nss, MODE_IX));
                            if (!collection.exists()) {
                                // For idempotency reasons, return success on delete operations.
                                if (entry.getOpType() == OpTypeEnum::kDelete) {
                                    return Status::OK();
                                }
                                uasserted(ErrorCodes::NamespaceNotFound,
                                          str::stream()
                                              << "cannot apply insert or update operation on a "
                                                 "non-existent namespace "
                                              << nss.toStringForErrorMsg() << ": "
                                              << mongo::redact(opObj));
                            }
                            AutoStatsTracker statsTracker(
                                opCtx,
                                nss,
                                Top::LockType::WriteLocked,
                                AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                DatabaseProfileSettings::get(opCtx->getServiceContext())
                                    .getDatabaseProfileLevel(nss.dbName()));

                            // We return the status rather than merely aborting so failure of CRUD
                            // ops doesn't stop the applyOps from trying to process the rest of the
                            // ops.  This is to leave the door open to parallelizing CRUD op
                            // application in the future.
                            const bool isDataConsistent = true;
                            return repl::applyOperation_inlock(opCtx,
                                                               collection,
                                                               ApplierOperation{&entry},
                                                               false, /* alwaysUpsert */
                                                               oplogApplicationMode,
                                                               isDataConsistent);
                        }

                        case OpTypeEnum::kNoop:
                            return Status::OK();
                    }
                    MONGO_UNREACHABLE;
                },
                oplogApplicationMode == repl::OplogApplication::Mode::kSecondary);
        } catch (const DBException& ex) {
            ab.append(false);
            result->append("applied", ++(*numApplied));
            result->append("code", ex.code());
            result->append("codeName", ErrorCodes::errorString(ex.code()));
            result->append("errmsg", ex.what());
            result->append("results", ab.arr());
            return ex.toStatus();
        }

        ab.append(status.isOK());
        if (!status.isOK()) {
            LOGV2(21064, "applyOps error applying", "error"_attr = status);
            errors++;
        }

        (*numApplied)++;

        if (MONGO_unlikely(applyOpsPauseBetweenOperations.shouldFail())) {
            applyOpsPauseBetweenOperations.pauseWhileSet();
        }
    }

    result->append("applied", *numApplied);
    result->append("results", ab.arr());

    if (errors != 0) {
        return Status(ErrorCodes::UnknownError, "applyOps had one or more errors applying ops");
    }

    return Status::OK();
}

}  // namespace

Status applyApplyOpsOplogEntry(OperationContext* opCtx,
                               const OplogEntry& entry,
                               repl::OplogApplication::Mode oplogApplicationMode) {
    invariant(!entry.shouldPrepare());
    BSONObjBuilder resultWeDontCareAbout;
    return applyOps(opCtx,
                    entry.getNss().dbName(),
                    entry.getObject(),
                    oplogApplicationMode,
                    &resultWeDontCareAbout);
}

Status applyOps(OperationContext* opCtx,
                const DatabaseName& dbName,
                const BSONObj& applyOpCmd,
                repl::OplogApplication::Mode oplogApplicationMode,
                BSONObjBuilder* result) {
    auto info = ApplyOpsCommandInfo::parse(applyOpCmd);

    int numApplied = 0;

    boost::optional<Lock::GlobalWrite> globalWriteLock;
    boost::optional<Lock::DBLock> dbWriteLock;

    uassert(
        ErrorCodes::BadValue, "applyOps command can't have 'prepare' field", !info.getPrepare());
    uassert(31056, "applyOps command can't have 'partialTxn' field.", !info.getPartialTxn());
    uassert(31240, "applyOps command can't have 'count' field.", !info.getCount());

    if (info.areOpsCrudOnly()) {
        dbWriteLock.emplace(opCtx, dbName, MODE_IX);
    } else {
        globalWriteLock.emplace(opCtx);
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool userInitiatedWritesAndNotPrimary =
        opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

    if (userInitiatedWritesAndNotPrimary)
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while applying ops to database "
                                    << dbName.toStringForErrorMsg());

    LOGV2_DEBUG(5854600,
                2,
                "applyOps command",
                "dbName"_attr = redact(toStringForLogging(dbName)),
                "cmd"_attr = redact(applyOpCmd));

    auto hasDropDatabase = std::any_of(
        info.getOperations().begin(), info.getOperations().end(), [](const BSONObj& op) {
            return op.getStringField("op") == "c" &&
                parseCommandType(op.getObjectField("o")) == OplogEntry::CommandType::kDropDatabase;
        });
    if (hasDropDatabase) {
        // Normally the contract for applyOps is to hold a global exclusive lock during
        // application of ops. However, dropDatabase must specially not hold locks because it
        // may need to await replication internally during application. Additionally, since
        // dropDatabase is abnormal in locking behavior, applyOps is only allowed to apply a
        // dropDatabase op singly, not in combination with additional ops.
        uassert(6275900,
                "dropDatabase in an applyOps must be the only entry",
                info.getOperations().size() == 1);
        globalWriteLock.reset();
    }
    return _applyOps(
        opCtx, info, dbName, oplogApplicationMode, result, &numApplied, nullptr /* opsBuilder */);
}

}  // namespace repl
}  // namespace mongo
