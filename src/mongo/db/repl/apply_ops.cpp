/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/repl/apply_ops.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto kPreconditionFieldName = "preCondition"_sd;

// If enabled, causes loop in _applyOps() to hang after applying current operation.
MONGO_FP_DECLARE(applyOpsPauseBetweenOperations);

/**
 * Return true iff the applyOpsCmd can be executed in a single WriteUnitOfWork.
 */
bool _areOpsCrudOnly(const BSONObj& applyOpCmd) {
    for (const auto& elem : applyOpCmd.firstElement().Obj()) {
        const char* names[] = {"ns", "op"};
        BSONElement fields[2];
        elem.Obj().getFields(2, names, fields);
        BSONElement& fieldNs = fields[0];
        BSONElement& fieldOp = fields[1];

        const char* opType = fieldOp.valuestrsafe();
        const StringData ns = fieldNs.valueStringData();

        // All atomic ops have an opType of length 1.
        if (opType[0] == '\0' || opType[1] != '\0')
            return false;

        // Only consider CRUD operations.
        switch (*opType) {
            case 'd':
            case 'n':
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

Status _applyOps(OperationContext* opCtx,
                 const std::string& dbName,
                 const BSONObj& applyOpCmd,
                 BSONObjBuilder* result,
                 int* numApplied) {
    BSONObj ops = applyOpCmd.firstElement().Obj();

    // apply
    *numApplied = 0;
    int errors = 0;

    BSONObjIterator i(ops);
    BSONArrayBuilder ab;
    const bool alwaysUpsert =
        applyOpCmd.hasField("alwaysUpsert") ? applyOpCmd["alwaysUpsert"].trueValue() : true;
    const bool haveWrappingWUOW = opCtx->lockState()->inAWriteUnitOfWork();

    while (i.more()) {
        BSONElement e = i.next();
        const BSONObj& opObj = e.Obj();

        // Ignore 'n' operations.
        const char* opType = opObj["op"].valuestrsafe();
        if (*opType == 'n')
            continue;

        const std::string ns = opObj["ns"].String();
        const NamespaceString nss{ns};

        // Need to check this here, or OldClientContext may fail an invariant.
        if (*opType != 'c' && !nss.isValid())
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};

        Status status(ErrorCodes::InternalError, "");

        if (haveWrappingWUOW) {
            invariant(opCtx->lockState()->isW());
            invariant(*opType != 'c');

            if (!dbHolder().get(opCtx, ns)) {
                throw DBException(
                    ErrorCodes::NamespaceNotFound,
                    "cannot create a database in atomic applyOps mode; will retry without "
                    "atomicity");
            }

            OldClientContext ctx(opCtx, ns);
            status = repl::applyOperation_inlock(opCtx, ctx.db(), opObj, alwaysUpsert);
            if (!status.isOK())
                return status;
        } else {
            try {
                writeConflictRetry(opCtx, "applyOps", ns, [&] {
                    if (*opType == 'c') {
                        invariant(opCtx->lockState()->isW());
                        status = repl::applyCommand_inlock(opCtx, opObj, true);
                    } else {
                        Lock::DBLock dbWriteLock(opCtx, nss.db(), MODE_IX);
                        Lock::CollectionLock lock(opCtx->lockState(), nss.ns(), MODE_IX);
                        OldClientContext ctx(opCtx, ns);
                        const char* names[] = {"o", "ns"};
                        BSONElement fields[2];
                        opObj.getFields(2, names, fields);
                        BSONElement& fieldO = fields[0];
                        BSONElement& fieldNs = fields[1];
                        const StringData ns = fieldNs.valueStringData();
                        NamespaceString requestNss{ns};

                        if (nss.isSystemDotIndexes()) {
                            BSONObj indexSpec;
                            NamespaceString indexNss;
                            std::tie(indexSpec, indexNss) =
                                repl::prepForApplyOpsIndexInsert(fieldO, opObj, requestNss);
                            BSONObjBuilder command;
                            command.append("createIndexes", indexNss.coll());
                            {
                                BSONArrayBuilder indexes(command.subarrayStart("indexes"));
                                indexes.append(indexSpec);
                                indexes.doneFast();
                            }
                            const BSONObj commandObj = command.done();

                            DBDirectClient client(opCtx);
                            BSONObj infoObj;
                            client.runCommand(nsToDatabase(ns), commandObj, infoObj);
                            status = getStatusFromCommandResult(infoObj);
                        } else {
                            status =
                                repl::applyOperation_inlock(opCtx, ctx.db(), opObj, alwaysUpsert);
                        }
                    }
                });
            } catch (const DBException& ex) {
                ab.append(false);
                result->append("applied", ++(*numApplied));
                result->append("code", ex.code());
                result->append("codeName", ErrorCodes::errorString(ex.code()));
                result->append("errmsg", ex.what());
                result->append("results", ab.arr());
                return Status(ErrorCodes::UnknownError, ex.what());
            }
        }

        ab.append(status.isOK());
        if (!status.isOK()) {
            log() << "applyOps error applying: " << status;
            errors++;
        }

        (*numApplied)++;

        if (MONGO_FAIL_POINT(applyOpsPauseBetweenOperations)) {
            // While holding a database lock under MMAPv1, we would be implicitly holding the
            // flush lock here. This would prevent other threads from acquiring the global
            // lock or any database locks. We release all locks temporarily while the fail
            // point is enabled to allow other threads to make progress.
            boost::optional<Lock::TempRelease> release;
            auto storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
            if (storageEngine->isMmapV1() && !opCtx->lockState()->isW()) {
                release.emplace(opCtx->lockState());
            }
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(applyOpsPauseBetweenOperations);
        }
    }

    result->append("applied", *numApplied);
    result->append("results", ab.arr());

    if (errors != 0) {
        return Status(ErrorCodes::UnknownError, "applyOps had one or more errors applying ops");
    }

    return Status::OK();
}

bool _hasPrecondition(const BSONObj& applyOpCmd) {
    return applyOpCmd[kPreconditionFieldName].type() == Array;
}

Status _checkPrecondition(OperationContext* opCtx,
                          const BSONObj& applyOpCmd,
                          BSONObjBuilder* result) {
    invariant(opCtx->lockState()->isW());
    invariant(_hasPrecondition(applyOpCmd));

    for (auto elem : applyOpCmd[kPreconditionFieldName].Obj()) {
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

        DBDirectClient db(opCtx);
        BSONObj realres = db.findOne(nss.ns(), preCondition["q"].Obj());

        // Get collection default collation.
        Database* database = dbHolder().get(opCtx, nss.db());
        if (!database) {
            return {ErrorCodes::NamespaceNotFound, "database in ns does not exist: " + nss.ns()};
        }
        Collection* collection = database->getCollection(opCtx, nss);
        if (!collection) {
            return {ErrorCodes::NamespaceNotFound, "collection in ns does not exist: " + nss.ns()};
        }
        const CollatorInterface* collator = collection->getDefaultCollator();

        // applyOps does not allow any extensions, such as $text, $where, $geoNear, $near,
        // $nearSphere, or $expr.
        Matcher matcher(preCondition["res"].Obj(), collator);
        if (!matcher.matches(realres)) {
            result->append("got", realres);
            result->append("whatFailed", preCondition);
            return {ErrorCodes::BadValue, "preCondition failed"};
        }
    }

    return Status::OK();
}
}  // namespace

Status applyOps(OperationContext* opCtx,
                const std::string& dbName,
                const BSONObj& applyOpCmd,
                BSONObjBuilder* result) {
    bool allowAtomic = false;
    uassertStatusOK(
        bsonExtractBooleanFieldWithDefault(applyOpCmd, "allowAtomic", true, &allowAtomic));
    auto areOpsCrudOnly = _areOpsCrudOnly(applyOpCmd);
    auto isAtomic = allowAtomic && areOpsCrudOnly;
    auto hasPrecondition = _hasPrecondition(applyOpCmd);

    if (hasPrecondition) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot use preCondition with {allowAtomic: false}.",
                allowAtomic);
        uassert(ErrorCodes::InvalidOptions,
                "Cannot use preCondition when operations include commands.",
                areOpsCrudOnly);
    }

    boost::optional<Lock::GlobalWrite> globalWriteLock;
    boost::optional<Lock::DBLock> dbWriteLock;

    // There's only one case where we are allowed to take the database lock instead of the global
    // lock - no preconditions; only CRUD ops; and non-atomic mode.
    if (!hasPrecondition && areOpsCrudOnly && !allowAtomic) {
        dbWriteLock.emplace(opCtx, dbName, MODE_IX);
    } else {
        globalWriteLock.emplace(opCtx);
    }

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(opCtx, dbName);

    if (userInitiatedWritesAndNotPrimary)
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while applying ops to database " << dbName);

    if (hasPrecondition) {
        invariant(isAtomic);
        auto status = _checkPrecondition(opCtx, applyOpCmd, result);
        if (!status.isOK()) {
            return status;
        }
    }

    int numApplied = 0;
    if (!isAtomic)
        return _applyOps(opCtx, dbName, applyOpCmd, result, &numApplied);

    // Perform write ops atomically
    invariant(globalWriteLock);
    try {
        writeConflictRetry(opCtx, "applyOps", dbName, [&] {
            BSONObjBuilder intermediateResult;
            WriteUnitOfWork wunit(opCtx);
            numApplied = 0;
            {
                // Suppress replication for atomic operations until end of applyOps.
                repl::UnreplicatedWritesBlock uwb(opCtx);
                uassertStatusOK(
                    _applyOps(opCtx, dbName, applyOpCmd, &intermediateResult, &numApplied));
            }
            // Generate oplog entry for all atomic ops collectively.
            if (opCtx->writesAreReplicated()) {
                // We want this applied atomically on slaves so we rewrite the oplog entry without
                // the pre-condition for speed.

                BSONObjBuilder cmdBuilder;

                for (auto elem : applyOpCmd) {
                    auto name = elem.fieldNameStringData();
                    if (name == kPreconditionFieldName)
                        continue;
                    if (name == "bypassDocumentValidation")
                        continue;
                    cmdBuilder.append(elem);
                }

                const BSONObj cmdRewritten = cmdBuilder.done();

                auto opObserver = getGlobalServiceContext()->getOpObserver();
                invariant(opObserver);
                opObserver->onApplyOps(opCtx, dbName, cmdRewritten);
            }
            wunit.commit();
            result->appendElements(intermediateResult.obj());
        });
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::NamespaceNotFound) {
            // Retry in non-atomic mode, since MMAP cannot implicitly create a new database
            // within an active WriteUnitOfWork.
            return _applyOps(opCtx, dbName, applyOpCmd, result, &numApplied);
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
        return Status(ErrorCodes::UnknownError, ex.what());
    }

    return Status::OK();
}

}  // namespace mongo
