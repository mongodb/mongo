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
#include "mongo/client/client_deprecated.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
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
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace repl {

constexpr StringData ApplyOps::kPreconditionFieldName;
constexpr StringData ApplyOps::kOplogApplicationModeFieldName;

namespace {

// If enabled, causes loop in _applyOps() to hang after applying current operation.
MONGO_FAIL_POINT_DEFINE(applyOpsPauseBetweenOperations);

Status _applyOps(OperationContext* opCtx,
                 const ApplyOpsCommandInfo& info,
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
    const bool haveWrappingWUOW = opCtx->lockState()->inAWriteUnitOfWork();

    // Apply each op in the given 'applyOps' command object.
    for (const auto& opObj : ops) {
        // Ignore 'n' operations.
        const char* opType = opObj.getStringField("op").rawData();
        if (*opType == 'n')
            continue;

        const NamespaceString nss(opObj["ns"].String());

        // Need to check this here, or OldClientContext may fail an invariant.
        if (*opType != 'c' && !nss.isValid())
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};

        Status status = Status::OK();

        if (haveWrappingWUOW) {
            // Only CRUD operations are allowed in atomic mode.
            invariant(*opType != 'c');

            // ApplyOps does not have the global writer lock when applying transaction
            // operations, so we need to acquire the DB and Collection locks.
            Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);

            // When processing an update on a non-existent collection, applyOperation_inlock()
            // returns UpdateOperationFailed on updates and allows the collection to be
            // implicitly created on upserts. We detect both cases here and fail early with
            // NamespaceNotFound.
            // Additionally for inserts, we fail early on non-existent collections.
            Lock::CollectionLock collectionLock(opCtx, nss, MODE_IX);
            auto collection =
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
            if (!collection && (*opType == 'i' || *opType == 'u')) {
                uasserted(
                    ErrorCodes::AtomicityFailure,
                    str::stream()
                        << "cannot apply insert or update operation on a non-existent namespace "
                        << nss.ns() << " in atomic applyOps mode: " << redact(opObj));
            }
            uassert(ErrorCodes::AtomicityFailure,
                    str::stream() << "cannot run atomic applyOps on namespace " << nss.ns()
                                  << " which has change stream pre- or post-images enabled",
                    !collection->isChangeStreamPreAndPostImagesEnabled());

            // Reject malformed or over-specified operations in an atomic applyOps.
            try {
                ReplOperation::parse(IDLParserErrorContext("applyOps"), opObj);
            } catch (...) {
                uasserted(ErrorCodes::AtomicityFailure,
                          str::stream() << "cannot apply a malformed or over-specified operation "
                                           "in atomic applyOps mode: "
                                        << redact(opObj) << "; will retry without atomicity: "
                                        << exceptionToStatus().toString());
            }

            BSONObjBuilder builder;
            builder.appendElements(opObj);

            // Create these required fields and populate them with dummy values before parsing the
            // BSONObj as an oplog entry.
            builder.append(OplogEntry::kTimestampFieldName, Timestamp());
            builder.append(OplogEntry::kWallClockTimeFieldName, Date_t());
            auto entry = OplogEntry::parse(builder.done());

            // Malformed operations should have already been caught and retried in non-atomic mode.
            invariant(entry.isOK());

            OldClientContext ctx(opCtx, nss.ns());

            const auto& op = entry.getValue();
            const bool isDataConsistent = true;
            status = repl::applyOperation_inlock(
                opCtx, ctx.db(), &op, alwaysUpsert, oplogApplicationMode, isDataConsistent);
            if (!status.isOK())
                return status;

            // Append completed op, including UUID if available, to 'opsBuilder'.
            if (opsBuilder) {
                if (opObj.hasField("ui") || !collection) {
                    // No changes needed to operation document.
                    opsBuilder->append(opObj);
                } else {
                    // Operation document has no "ui" field and collection has a UUID.
                    auto uuid = collection->uuid();
                    BSONObjBuilder opBuilder;
                    opBuilder.appendElements(opObj);
                    uuid.appendToBuilder(&opBuilder, "ui");
                    opsBuilder->append(opBuilder.obj());
                }
            }
        } else {
            try {
                status = writeConflictRetry(
                    opCtx,
                    "applyOps",
                    nss.ns(),
                    [opCtx, nss, opObj, opType, alwaysUpsert, oplogApplicationMode, &info] {
                        BSONObjBuilder builder;
                        builder.appendElements(opObj);
                        if (!builder.hasField(OplogEntry::kTimestampFieldName)) {
                            builder.append(OplogEntry::kTimestampFieldName, Timestamp());
                        }
                        if (!builder.hasField(OplogEntry::kHashFieldName)) {
                            builder.append(OplogEntry::kHashFieldName, 0LL);
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
                                uassertStatusOK(
                                    applyCommand_inlock(opCtx, entry, oplogApplicationMode));
                                return Status::OK();
                            }
                            invariant(opCtx->lockState()->isW());
                            uassertStatusOK(
                                applyCommand_inlock(opCtx, entry, oplogApplicationMode));
                            return Status::OK();
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
                                          << nss.ns() << ": " << mongo::redact(opObj));
                        }

                        OldClientContext ctx(opCtx, nss.ns());

                        // We return the status rather than merely aborting so failure of CRUD
                        // ops doesn't stop the applyOps from trying to process the rest of the
                        // ops.  This is to leave the door open to parallelizing CRUD op
                        // application in the future.
                        const bool isDataConsistent = true;
                        return repl::applyOperation_inlock(opCtx,
                                                           ctx.db(),
                                                           &entry,
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

Status _checkPrecondition(OperationContext* opCtx,
                          const std::vector<BSONObj>& preConditions,
                          BSONObjBuilder* result) {
    invariant(opCtx->lockState()->isW());

    for (const auto& preCondition : preConditions) {
        if (preCondition["ns"].type() != BSONType::String) {
            return {ErrorCodes::InvalidNamespace,
                    str::stream() << "ns in preCondition must be a string, but found type: "
                                  << typeName(preCondition["ns"].type())};
        }
        const NamespaceString nss(preCondition["ns"].valueStringData());
        if (!nss.isValid()) {
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};
        }

        DBDirectClient db(opCtx);
        // The preconditions come in "q: {{query: {...}, orderby: ..., etc.}}" format. This format
        // is no longer used either internally or over the wire in other contexts. We are using a
        // legacy API from 'client_deprecated' in order to parse this format and convert it into the
        // corresponding find command.
        FindCommandRequest findCmd{nss};
        client_deprecated::initFindFromLegacyOptions(preCondition["q"].Obj(), 0, &findCmd);
        BSONObj realres = db.findOne(std::move(findCmd));

        // Get collection default collation.
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto database = databaseHolder->getDb(opCtx, nss.dbName());
        if (!database) {
            return {ErrorCodes::NamespaceNotFound, "database in ns does not exist: " + nss.ns()};
        }
        CollectionPtr collection =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        if (!collection) {
            return {ErrorCodes::NamespaceNotFound, "collection in ns does not exist: " + nss.ns()};
        }
        const CollatorInterface* collator = collection->getDefaultCollator();

        // applyOps does not allow any extensions, such as $text, $where, $geoNear, $near,
        // $nearSphere, or $expr.
        boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtx, CollatorInterface::cloneCollator(collator), nss));
        Matcher matcher(preCondition["res"].Obj(), std::move(expCtx));
        if (!matcher.matches(realres)) {
            result->append("got", realres);
            result->append("whatFailed", preCondition);
            return {ErrorCodes::BadValue, "preCondition failed"};
        }
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
                    entry.getNss().db().toString(),
                    entry.getObject(),
                    oplogApplicationMode,
                    &resultWeDontCareAbout);
}

Status applyOps(OperationContext* opCtx,
                const std::string& dbName,
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

    // There's one case where we are allowed to take the database lock instead of the global
    // lock - no preconditions; only CRUD ops; and non-atomic mode.
    if (!info.getPreCondition() && info.areOpsCrudOnly() && !info.getAllowAtomic()) {
        // TODO SERVER-62880 Once the dbName is of type DatabaseName, pass it directly to the DBlock
        DatabaseName databaseName(boost::none, dbName);
        dbWriteLock.emplace(opCtx, databaseName, MODE_IX);
    } else {
        globalWriteLock.emplace(opCtx);
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool userInitiatedWritesAndNotPrimary =
        opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

    if (userInitiatedWritesAndNotPrimary)
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while applying ops to database " << dbName);

    if (auto preCondition = info.getPreCondition()) {
        invariant(info.isAtomic());
        auto status = _checkPrecondition(opCtx, *preCondition, result);
        if (!status.isOK()) {
            return status;
        }
    }

    LOGV2_DEBUG(5854600,
                2,
                "applyOps command",
                "dbName"_attr = redact(dbName),
                "cmd"_attr = redact(applyOpCmd));

    if (!info.isAtomic()) {
        auto hasDropDatabase = std::any_of(
            info.getOperations().begin(), info.getOperations().end(), [](const BSONObj& op) {
                return op.getStringField("op") == "c" &&
                    parseCommandType(op.getObjectField("o")) ==
                    OplogEntry::CommandType::kDropDatabase;
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
        return _applyOps(opCtx, info, oplogApplicationMode, result, &numApplied, nullptr);
    }

    // Perform write ops atomically
    invariant(globalWriteLock);

    try {
        writeConflictRetry(opCtx, "applyOps", dbName, [&] {
            BSONObjBuilder intermediateResult;
            std::unique_ptr<BSONArrayBuilder> opsBuilder;

            // If we were to replicate the original applyOps operation we received, we could
            // replicate an applyOps that includes no-op writes. Oplog readers, like change streams,
            // would then see entries for writes that did not happen. To work around this, we group
            // all writes in this WUOW into a new applyOps entry so that we only replicate writes
            // that actually happen.
            // Note that the applyOps command doesn't update config.transactions for retryable
            // writes, nor does it support change stream pre- and post-images.

            WriteUnitOfWork wunit(opCtx, true /*groupOplogEntries*/);
            numApplied = 0;
            uassertStatusOK(_applyOps(
                opCtx, info, oplogApplicationMode, &intermediateResult, &numApplied, nullptr));
            wunit.commit();
            result->appendElements(intermediateResult.obj());
        });
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::AtomicityFailure) {
            // Retry in non-atomic mode.
            return _applyOps(opCtx, info, oplogApplicationMode, result, &numApplied, nullptr);
        }
        BSONArrayBuilder ab;
        ++numApplied;
        for (int j = 0; j < numApplied; j++)
            ab.append(false);
        result->append("applied", numApplied);
        result->append("code", ex.code());
        result->append("codeName", ErrorCodes::errorString(ex.code()));
        result->append("errmsg", ex.what());
        result->append("results", ab.arr());
        return ex.toStatus();
    }

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
