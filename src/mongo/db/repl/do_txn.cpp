/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/repl/do_txn.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

constexpr StringData DoTxn::kPreconditionFieldName;

namespace {

// If enabled, causes loop in _doTxn() to hang after applying current operation.
MONGO_FAIL_POINT_DEFINE(doTxnPauseBetweenOperations);

/**
 * Return true iff the doTxnCmd can be executed in a single WriteUnitOfWork.
 */
bool _areOpsCrudOnly(const BSONObj& doTxnCmd) {
    for (const auto& elem : doTxnCmd.firstElement().Obj()) {
        const char* names[] = {"ns", "op"};
        BSONElement fields[2];
        elem.Obj().getFields(2, names, fields);
        BSONElement& fieldNs = fields[0];
        BSONElement& fieldOp = fields[1];

        const char* opType = fieldOp.valuestrsafe();
        const StringData ns = fieldNs.valuestrsafe();

        // All atomic ops have an opType of length 1.
        if (opType[0] == '\0' || opType[1] != '\0')
            return false;

        // Only consider CRUD operations.
        switch (*opType) {
            case 'd':
            case 'u':
                break;
            case 'i':
                if (nsToCollectionSubstring(ns) != "system.indexes")
                    break;
            // Fallthrough.
            default:
                return false;
        }
    }

    return true;
}

Status _doTxn(OperationContext* opCtx,
              const std::string& dbName,
              const BSONObj& doTxnCmd,
              BSONObjBuilder* result,
              int* numApplied) {
    BSONObj ops = doTxnCmd.firstElement().Obj();
    // apply
    *numApplied = 0;
    int errors = 0;

    BSONObjIterator i(ops);
    BSONArrayBuilder ab;
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // Apply each op in the given 'doTxn' command object.
    while (i.more()) {
        BSONElement e = i.next();
        const BSONObj& opObj = e.Obj();

        NamespaceString nss(opObj["ns"].String());

        // Need to check this here, or OldClientContext may fail an invariant.
        if (!nss.isValid())
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};

        Status status(ErrorCodes::InternalError, "");

        AutoGetDb autoDb(opCtx, nss.db(), MODE_IX);
        auto db = autoDb.getDb();
        if (!db) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot apply insert, delete, or update operation on a "
                                       "non-existent namespace "
                                    << nss.ns()
                                    << ": "
                                    << mongo::redact(opObj));
        }

        if (opObj.hasField("ui")) {
            auto uuidStatus = UUID::parse(opObj["ui"]);
            uassertStatusOK(uuidStatus.getStatus());
            // If "ui" is present, it overrides "nss" for the collection name.
            nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(uuidStatus.getValue());
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "cannot find collection uuid " << uuidStatus.getValue(),
                    !nss.isEmpty());
        }
        Lock::CollectionLock collLock(opCtx->lockState(), nss.ns(), MODE_IX);
        auto collection = db->getCollection(opCtx, nss);

        // When processing an update on a non-existent collection, applyOperation_inlock()
        // returns UpdateOperationFailed on updates and allows the collection to be
        // implicitly created on upserts. We detect both cases here and fail early with
        // NamespaceNotFound.
        // Additionally for inserts, we fail early on non-existent collections.
        if (!collection && db->getViewCatalog()->lookup(opCtx, nss.ns())) {
            uasserted(ErrorCodes::CommandNotSupportedOnView,
                      str::stream() << "doTxn not supported on a view: " << redact(opObj));
        }
        if (!collection) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot apply operation on a non-existent namespace "
                                    << nss.ns()
                                    << " with doTxn: "
                                    << redact(opObj));
        }

        // Setting alwaysUpsert to true makes sense only during oplog replay, and doTxn commands
        // should not be executed during oplog replay.
        const bool alwaysUpsert = false;
        status = repl::applyOperation_inlock(
            opCtx, db, opObj, alwaysUpsert, repl::OplogApplication::Mode::kApplyOpsCmd);
        if (!status.isOK())
            return status;

        ab.append(status.isOK());
        if (!status.isOK()) {
            log() << "doTxn error applying: " << status;
            errors++;
        }

        (*numApplied)++;

        if (MONGO_FAIL_POINT(doTxnPauseBetweenOperations)) {
            // While holding a database lock under MMAPv1, we would be implicitly holding the
            // flush lock here. This would prevent other threads from acquiring the global
            // lock or any database locks. We release all locks temporarily while the fail
            // point is enabled to allow other threads to make progress.
            boost::optional<Lock::TempRelease> release;
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            if (storageEngine->isMmapV1() && !opCtx->lockState()->isW()) {
                release.emplace(opCtx->lockState());
            }
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(doTxnPauseBetweenOperations);
        }
    }

    result->append("applied", *numApplied);
    result->append("results", ab.arr());

    if (errors != 0) {
        return Status(ErrorCodes::UnknownError, "doTxn had one or more errors applying ops");
    }

    return Status::OK();
}

bool _hasPrecondition(const BSONObj& doTxnCmd) {
    return doTxnCmd[DoTxn::kPreconditionFieldName].type() == Array;
}

Status _checkPrecondition(OperationContext* opCtx,
                          const BSONObj& doTxnCmd,
                          BSONObjBuilder* result) {
    // Precondition check must be done in a write unit of work to make sure it's
    // sharing the same snapshot as the writes.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    invariant(_hasPrecondition(doTxnCmd));

    for (auto elem : doTxnCmd[DoTxn::kPreconditionFieldName].Obj()) {
        auto preCondition = elem.Obj();
        if (preCondition["ns"].type() != BSONType::String) {
            return {ErrorCodes::InvalidNamespace,
                    str::stream() << "ns in preCondition must be a string, but found type: "
                                  << typeName(preCondition["ns"].type())};
        }
        const NamespaceString nss(preCondition["ns"].valueStringData());
        if (!nss.isValid()) {
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};
        }

        // Even if snapshot isolation is provided, database catalog still needs locking.
        // Only X and IX mode locks are stashed by the WriteUnitOfWork and IS->IX upgrade
        // is not supported, so we can not use IS mode here.
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);

        Database* database = autoColl.getDb();
        if (!database) {
            return {ErrorCodes::NamespaceNotFound, "database in ns does not exist: " + nss.ns()};
        }
        Collection* collection = autoColl.getCollection();
        if (!collection) {
            return {ErrorCodes::NamespaceNotFound, "collection in ns does not exist: " + nss.ns()};
        }

        BSONObj realres;
        auto qrStatus = QueryRequest::fromLegacyQuery(nss, preCondition["q"].Obj(), {}, 0, 0, 0);
        if (!qrStatus.isOK()) {
            return qrStatus.getStatus();
        }
        auto recordId = Helpers::findOne(
            opCtx, autoColl.getCollection(), std::move(qrStatus.getValue()), false);
        if (!recordId.isNull()) {
            realres = collection->docFor(opCtx, recordId).value();
        }


        // Get collection default collation.
        const CollatorInterface* collator = collection->getDefaultCollator();

        boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, collator));
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

Status doTxn(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& doTxnCmd,
             BSONObjBuilder* result) {
    auto txnNumber = opCtx->getTxnNumber();
    uassert(ErrorCodes::InvalidOptions, "doTxn can only be run with a transaction ID.", txnNumber);
    auto* session = OperationContextSession::get(opCtx);
    uassert(ErrorCodes::InvalidOptions, "doTxn must be run within a session", session);
    invariant(session->inMultiDocumentTransaction());
    invariant(opCtx->getWriteUnitOfWork());
    uassert(
        ErrorCodes::InvalidOptions, "doTxn supports only CRUD opts.", _areOpsCrudOnly(doTxnCmd));
    auto hasPrecondition = _hasPrecondition(doTxnCmd);


    // Acquire global lock in IX mode so that the replication state check will remain valid.
    Lock::GlobalLock globalLock(opCtx, MODE_IX);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool userInitiatedWritesAndNotPrimary =
        opCtx->writesAreReplicated() && !replCoord->canAcceptWritesForDatabase(opCtx, dbName);

    if (userInitiatedWritesAndNotPrimary)
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while applying ops to database " << dbName);

    int numApplied = 0;

    try {
        BSONObjBuilder intermediateResult;

        // The transaction takes place in a global unit of work, so the precondition check
        // and the writes will share the same snapshot.
        if (hasPrecondition) {
            uassertStatusOK(_checkPrecondition(opCtx, doTxnCmd, result));
        }

        numApplied = 0;
        uassertStatusOK(_doTxn(opCtx, dbName, doTxnCmd, &intermediateResult, &numApplied));
        session->commitTransaction(opCtx);
        result->appendElements(intermediateResult.obj());
    } catch (const DBException& ex) {
        session->abortActiveTransaction(opCtx);
        BSONArrayBuilder ab;
        ++numApplied;
        for (int j = 0; j < numApplied; j++)
            ab.append(false);
        result->append("applied", numApplied);
        result->append("code", ex.code());
        result->append("codeName", ErrorCodes::errorString(ex.code()));
        result->append("errmsg", ex.what());
        result->append("results", ab.arr());
        return Status(ErrorCodes::UnknownError, ex.what());
    }

    return Status::OK();
}

}  // namespace mongo
