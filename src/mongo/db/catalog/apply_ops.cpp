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

#include "mongo/db/catalog/apply_ops.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
/**
 * Return true iff the applyOpsCmd can be executed in a single WriteUnitOfWork.
 */
bool canBeAtomic(const BSONObj& applyOpCmd) {
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

Status _applyOps(OperationContext* txn,
                 const std::string& dbName,
                 const BSONObj& applyOpCmd,
                 BSONObjBuilder* result,
                 int* numApplied) {
    dassert(txn->lockState()->isLockHeldForMode(
        ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL), MODE_X));

    bool shouldReplicateWrites = txn->writesAreReplicated();
    txn->setReplicatedWrites(false);
    BSONObj ops = applyOpCmd.firstElement().Obj();

    // apply
    *numApplied = 0;
    int errors = 0;

    BSONObjIterator i(ops);
    BSONArrayBuilder ab;
    const bool alwaysUpsert =
        applyOpCmd.hasField("alwaysUpsert") ? applyOpCmd["alwaysUpsert"].trueValue() : true;
    const bool haveWrappingWUOW = txn->lockState()->inAWriteUnitOfWork();

    while (i.more()) {
        BSONElement e = i.next();
        const BSONObj& opObj = e.Obj();

        // Ignore 'n' operations.
        const char* opType = opObj["op"].valuestrsafe();
        if (*opType == 'n')
            continue;

        const std::string ns = opObj["ns"].String();

        // Need to check this here, or OldClientContext may fail an invariant.
        if (*opType != 'c' && !NamespaceString(ns).isValid())
            return {ErrorCodes::InvalidNamespace, "invalid ns: " + ns};

        Status status(ErrorCodes::InternalError, "");

        if (haveWrappingWUOW) {
            invariant(*opType != 'c');

            OldClientContext ctx(txn, ns);
            status = repl::applyOperation_inlock(txn, ctx.db(), opObj, alwaysUpsert);
            if (!status.isOK())
                return status;
            logOpForDbHash(txn, ns.c_str());
        } else {
            try {
                // Run operations under a nested lock as a hack to prevent yielding.
                //
                // The list of operations is supposed to be applied atomically; yielding
                // would break atomicity by allowing an interruption or a shutdown to occur
                // after only some operations are applied.  We are already locked globally
                // at this point, so taking a DBLock on the namespace creates a nested lock,
                // and yields are disallowed for operations that hold a nested lock.
                //
                // We do not have a wrapping WriteUnitOfWork so it is possible for a journal
                // commit to happen with a subset of ops applied.
                Lock::GlobalWrite globalWriteLockDisallowTempRelease(txn->lockState());

                // Ensures that yielding will not happen (see the comment above).
                DEV {
                    Locker::LockSnapshot lockSnapshot;
                    invariant(!txn->lockState()->saveLockStateAndUnlock(&lockSnapshot));
                };

                MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                    if (*opType == 'c') {
                        status = repl::applyCommand_inlock(txn, opObj);
                    } else {
                        OldClientContext ctx(txn, ns);

                        status = repl::applyOperation_inlock(txn, ctx.db(), opObj, alwaysUpsert);
                    }
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "applyOps", ns);
            } catch (const DBException& ex) {
                ab.append(false);
                result->append("applied", ++(*numApplied));
                result->append("code", ex.getCode());
                result->append("errmsg", ex.what());
                result->append("results", ab.arr());
                return Status(ErrorCodes::UnknownError, "");
            }
            WriteUnitOfWork wuow(txn);
            logOpForDbHash(txn, ns.c_str());
            wuow.commit();
        }

        ab.append(status.isOK());
        if (!status.isOK()) {
            errors++;
        }

        (*numApplied)++;
    }

    result->append("applied", *numApplied);
    result->append("results", ab.arr());
    txn->setReplicatedWrites(shouldReplicateWrites);

    if (txn->writesAreReplicated()) {
        // We want this applied atomically on slaves
        // so we re-wrap without the pre-condition for speed

        std::string tempNS = str::stream() << dbName << ".$cmd";

        // TODO: possibly use mutable BSON to remove preCondition field
        // once it is available
        BSONObjBuilder cmdBuilder;

        for (auto elem : applyOpCmd) {
            auto name = elem.fieldNameStringData();
            if (name == "preCondition")
                continue;
            if (name == "bypassDocumentValidation")
                continue;
            cmdBuilder.append(elem);
        }

        const BSONObj cmdRewritten = cmdBuilder.done();

        if (haveWrappingWUOW) {
            getGlobalServiceContext()->getOpObserver()->onApplyOps(txn, tempNS, cmdRewritten);
        } else {
            // When executing applyOps outside of a wrapping WriteUnitOfWOrk, always logOp the
            // command regardless of whether the individial ops succeeded and rely on any failures
            // to also on secondaries. This isn't perfect, but it's what the command has always done
            // and is part of its "correct" behavior.
            while (true) {
                try {
                    WriteUnitOfWork wunit(txn);
                    getGlobalServiceContext()->getOpObserver()->onApplyOps(
                        txn, tempNS, cmdRewritten);
                    wunit.commit();
                    break;
                } catch (const WriteConflictException& wce) {
                    LOG(2) << "WriteConflictException while logging applyOps command, retrying.";
                    txn->recoveryUnit()->abandonSnapshot();
                    continue;
                }
            }
        }
    }

    if (errors != 0) {
        return Status(ErrorCodes::UnknownError, "");
    }

    return Status::OK();
}

Status preconditionOK(OperationContext* txn, const BSONObj& applyOpCmd, BSONObjBuilder* result) {
    dassert(txn->lockState()->isLockHeldForMode(
        ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL), MODE_X));

    if (applyOpCmd["preCondition"].type() == Array) {
        BSONObjIterator i(applyOpCmd["preCondition"].Obj());
        while (i.more()) {
            BSONObj preCondition = i.next().Obj();
            if (preCondition["ns"].type() != BSONType::String) {
                return {ErrorCodes::InvalidNamespace,
                        str::stream() << "ns in preCondition must be a string, but found type: "
                                      << typeName(preCondition["ns"].type())};
            }
            const NamespaceString nss(preCondition["ns"].valueStringData());
            if (!nss.isValid()) {
                return {ErrorCodes::InvalidNamespace, "invalid ns: " + nss.ns()};
            }

            DBDirectClient db(txn);
            BSONObj realres = db.findOne(nss.ns(), preCondition["q"].Obj());

            // Get collection default collation.
            Database* database = dbHolder().get(txn, nss.db());
            if (!database) {
                return {ErrorCodes::NamespaceNotFound,
                        "database in ns does not exist: " + nss.ns()};
            }
            Collection* collection = database->getCollection(nss.ns());
            if (!collection) {
                return {ErrorCodes::NamespaceNotFound,
                        "collection in ns does not exist: " + nss.ns()};
            }
            const CollatorInterface* collator = collection->getDefaultCollator();

            // Apply-ops would never have a $where/$text matcher. Using the "DisallowExtensions"
            // callback ensures that parsing will throw an error if $where or $text are found.
            Matcher matcher(
                preCondition["res"].Obj(), ExtensionsCallbackDisallowExtensions(), collator);
            if (!matcher.matches(realres)) {
                result->append("got", realres);
                result->append("whatFailed", preCondition);
                return {ErrorCodes::BadValue, "preCondition failed"};
            }
        }
    }
    return Status::OK();
}
}  // namespace

Status applyOps(OperationContext* txn,
                const std::string& dbName,
                const BSONObj& applyOpCmd,
                BSONObjBuilder* result) {
    // SERVER-4328 todo : is global ok or does this take a long time? i believe multiple
    // ns used so locking individually requires more analysis
    ScopedTransaction scopedXact(txn, MODE_X);
    Lock::GlobalWrite globalWriteLock(txn->lockState());

    bool userInitiatedWritesAndNotPrimary = txn->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbName);

    if (userInitiatedWritesAndNotPrimary)
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while applying ops to database " << dbName);

    Status preconditionStatus = preconditionOK(txn, applyOpCmd, result);
    if (!preconditionStatus.isOK()) {
        return preconditionStatus;
    }

    int numApplied = 0;
    if (!canBeAtomic(applyOpCmd))
        return _applyOps(txn, dbName, applyOpCmd, result, &numApplied);

    // Perform write ops atomically
    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wunit(txn);
            numApplied = 0;
            uassertStatusOK(_applyOps(txn, dbName, applyOpCmd, result, &numApplied));
            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "applyOps", dbName);
    } catch (const DBException& ex) {
        BSONArrayBuilder ab;
        ++numApplied;
        for (int j = 0; j < numApplied; j++)
            ab.append(false);
        result->append("applied", numApplied);
        result->append("code", ex.getCode());
        result->append("errmsg", ex.what());
        result->append("results", ab.arr());
        return Status(ErrorCodes::UnknownError, "");
    }

    return Status::OK();
}
}  // namespace mongo
