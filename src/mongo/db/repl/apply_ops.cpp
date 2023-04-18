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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/fail_point.h"

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
    const auto& alwaysUpsert = info.getAlwaysUpsert();
    // Apply each op in the given 'applyOps' command object.
    for (const auto& opObj : ops) {
        // Ignore 'n' operations.
        const char* opType = opObj.getStringField("op").rawData();
        if (*opType == 'n')
            continue;

        const NamespaceString nss(
            NamespaceStringUtil::deserialize(dbName.tenantId(), opObj["ns"].String()));

        // Need to check this here, or OldClientContext may fail an invariant.
        if (*opType != 'c' && !nss.isValid())
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.toStringForErrorMsg()};

        Status status = Status::OK();

        try {
            status = writeConflictRetry(
                opCtx,
                "applyOps",
                nss.ns(),
                [opCtx, nss, opObj, opType, alwaysUpsert, oplogApplicationMode, &info, &dbName] {
                    BSONObjBuilder builder;
                    // Remove 'hash' field if it is set. A bit slow as it rebuilds the object.
                    // TODO(SERVER-69062): Remove this step.
                    auto opObjectWithoutHash = opObj;
                    if (opObj.hasField(OplogEntry::kHashFieldName)) {
                        opObjectWithoutHash = opObj.removeField(OplogEntry::kHashFieldName);
                    }

                    builder.appendElements(opObjectWithoutHash);
                    if (!builder.hasField(OplogEntry::kTimestampFieldName)) {
                        builder.append(OplogEntry::kTimestampFieldName, Timestamp());
                    }
                    if (!builder.hasField(OplogEntry::kWallClockTimeFieldName)) {
                        builder.append(OplogEntry::kWallClockTimeFieldName, Date_t());
                    }
                    auto entry = uassertStatusOK(OplogEntry::parse(builder.done()));

                    if (*opType == 'c') {
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
                        invariant(opCtx->lockState()->isW());
                        uassertStatusOK(applyCommand_inlock(
                            opCtx, ApplierOperation{&entry}, oplogApplicationMode));
                        return Status::OK();
                    }

                    // If the namespace and uuid passed into applyOps point to different
                    // namespaces, throw an error.
                    auto catalog = CollectionCatalog::get(opCtx);
                    if (opObjectWithoutHash.hasField("ui")) {
                        auto uuid = UUID::parse(opObjectWithoutHash["ui"]).getValue();
                        auto nssFromUuid = catalog->lookupNSSByUUID(opCtx, uuid);
                        if (nssFromUuid != nss) {
                            return Status{ErrorCodes::Error(3318200),
                                          str::stream()
                                              << "Namespace '" << nss.toStringForErrorMsg()
                                              << "' and UUID '" << uuid.toString()
                                              << "' point to different collections"};
                        }
                    }

                    AutoGetCollection autoColl(
                        opCtx, nss, fixLockModeForSystemDotViewsChanges(nss, MODE_IX));
                    if (!autoColl.getCollection()) {
                        // For idempotency reasons, return success on delete operations.
                        if (*opType == 'd') {
                            return Status::OK();
                        }
                        uasserted(ErrorCodes::NamespaceNotFound,
                                  str::stream()
                                      << "cannot apply insert or update operation on a "
                                         "non-existent namespace "
                                      << nss.toStringForErrorMsg() << ": " << mongo::redact(opObj));
                    }

                    OldClientContext ctx(opCtx, nss);

                    // We return the status rather than merely aborting so failure of CRUD
                    // ops doesn't stop the applyOps from trying to process the rest of the
                    // ops.  This is to leave the door open to parallelizing CRUD op
                    // application in the future.
                    const bool isDataConsistent = true;
                    return repl::applyOperation_inlock(opCtx,
                                                       ctx.db(),
                                                       ApplierOperation{&entry},
                                                       alwaysUpsert,
                                                       oplogApplicationMode,
                                                       isDataConsistent);
                });
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
            LOGV2(21064,
                  "applyOps error applying: {error}",
                  "applyOps error applying",
                  "error"_attr = status);
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
    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !replCoord->canAcceptWritesForDatabase(opCtx, dbName.toStringWithTenantId());

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
