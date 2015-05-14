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

#include "mongo/db/client.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {
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

        if (userInitiatedWritesAndNotPrimary) {
            return Status(ErrorCodes::NotMaster,
                    str::stream() << "Not primary while applying ops to database " << dbName);
        }

        bool shouldReplicateWrites = txn->writesAreReplicated();
        txn->setReplicatedWrites(false);
        BSONObj ops = applyOpCmd.firstElement().Obj();
        // Preconditions check reads the database state, so needs to be done locked
        if (applyOpCmd["preCondition"].type() == Array) {
            BSONObjIterator i(applyOpCmd["preCondition"].Obj());
            while (i.more()) {
                BSONObj f = i.next().Obj();

                DBDirectClient db(txn);
                BSONObj realres = db.findOne(f["ns"].String() , f["q"].Obj());

                // Apply-ops would never have a $where matcher, so use the default callback,
                // which will throw an error if $where is found.
                Matcher m(f["res"].Obj());
                if (! m.matches(realres)) {
                    result->append("got" , realres);
                    result->append("whatFailed" , f);
                    txn->setReplicatedWrites(shouldReplicateWrites);
                    return Status(ErrorCodes::BadValue, "pre-condition failed");
                }
            }
        }

        // apply
        int num = 0;
        int errors = 0;
        
        BSONObjIterator i(ops);
        BSONArrayBuilder ab;
        const bool alwaysUpsert = applyOpCmd.hasField("alwaysUpsert") ?
                applyOpCmd["alwaysUpsert"].trueValue() : true;
        
        while (i.more()) {
            BSONElement e = i.next();
            const BSONObj& temp = e.Obj();

            // Ignore 'n' operations.
            const char *opType = temp["op"].valuestrsafe();
            if (*opType == 'n') continue;

            const std::string ns = temp["ns"].String();

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
            // TODO figure out what to do about this.
            Lock::GlobalWrite globalWriteLockDisallowTempRelease(txn->lockState());

            // Ensures that yielding will not happen (see the comment above).
            DEV {
                Locker::LockSnapshot lockSnapshot;
                invariant(!txn->lockState()->saveLockStateAndUnlock(&lockSnapshot));
            };

            Status status(ErrorCodes::InternalError, "");

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                if (*opType == 'c') {
                    status = repl::applyCommand_inlock(txn, temp);
                    break;
                }
                else {
                    OldClientContext ctx(txn, ns);

                    status = repl::applyOperation_inlock(txn, ctx.db(), temp, alwaysUpsert);
                    break;
                }
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "applyOps", ns);

            ab.append(status.isOK());
            if (!status.isOK()) {
                errors++;
            }

            num++;

            WriteUnitOfWork wuow(txn);
            logOpForDbHash(txn, ns.c_str());
            wuow.commit();
        }

        result->append("applied" , num);
        result->append("results" , ab.arr());
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
                if (name == "preCondition") continue;
                if (name == "bypassDocumentValidation") continue;
                cmdBuilder.append(elem);
            }

            const BSONObj cmdRewritten = cmdBuilder.done();

            // We currently always logOp the command regardless of whether the individial ops
            // succeeded and rely on any failures to also happen on secondaries. This isn't
            // perfect, but it's what the command has always done and is part of its "correct"
            // behavior.
            while (true) {
                try {
                    WriteUnitOfWork wunit(txn);
                    getGlobalServiceContext()->getOpObserver()->onApplyOps(txn,
                                                                           tempNS,
                                                                           cmdRewritten);
                    wunit.commit();
                    break;
                }
                catch (const WriteConflictException& wce) {
                    LOG(2) <<
                        "WriteConflictException while logging applyOps command, retrying.";
                    txn->recoveryUnit()->commitAndRestart();
                    continue;
                }
            }
        }

        if (errors != 0) {
            return Status(ErrorCodes::UnknownError, "");
        }

        return Status::OK();
    }

} // namespace mongo
