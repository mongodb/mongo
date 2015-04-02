/**
*    Copyright (C) 2008 10gen Inc.
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

#include <sstream>
#include <string>
#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/dbhash.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/service_context.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::string;
    using std::stringstream;

    class ApplyOpsCmd : public Command {
    public:
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return true; }

        ApplyOpsCmd() : Command( "applyOps" ) {}
        virtual void help( stringstream &help ) const {
            help << "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , res : ... } ] }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // applyOps can do pretty much anything, so require all privileges.
            RoleGraph::generateUniversalPrivileges(out);
        }
        virtual bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

            if ( cmdObj.firstElement().type() != Array ) {
                errmsg = "ops has to be an array";
                return false;
            }

            BSONObj ops = cmdObj.firstElement().Obj();

            {
                // check input
                BSONObjIterator i( ops );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if (!_checkOperation(e, errmsg)) {
                        return false;
                    }
                }
            }

            // SERVER-4328 todo : is global ok or does this take a long time? i believe multiple 
            // ns used so locking individually requires more analysis
            ScopedTransaction scopedXact(txn, MODE_X);
            Lock::GlobalWrite globalWriteLock(txn->lockState());

            if (!fromRepl &&
                !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname)) {
                return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                    << "Not primary while applying ops to database " << dbname));
            }

            // Preconditions check reads the database state, so needs to be done locked
            if ( cmdObj["preCondition"].type() == Array ) {
                BSONObjIterator i( cmdObj["preCondition"].Obj() );
                while ( i.more() ) {
                    BSONObj f = i.next().Obj();

                    DBDirectClient db( txn );
                    BSONObj realres = db.findOne( f["ns"].String() , f["q"].Obj() );

                    // Apply-ops would never have a $where matcher, so use the default callback,
                    // which will throw an error if $where is found.
                    Matcher m(f["res"].Obj());
                    if ( ! m.matches( realres ) ) {
                        result.append( "got" , realres );
                        result.append( "whatFailed" , f );
                        errmsg = "pre-condition failed";
                        return false;
                    }
                }
            }

            // apply
            int num = 0;
            int errors = 0;
            
            BSONObjIterator i( ops );
            BSONArrayBuilder ab;
            const bool alwaysUpsert = cmdObj.hasField("alwaysUpsert") ?
                    cmdObj["alwaysUpsert"].trueValue() : true;
            
            while ( i.more() ) {
                BSONElement e = i.next();
                const BSONObj& temp = e.Obj();

                // Ignore 'n' operations.
                const char *opType = temp["op"].valuestrsafe();
                if (*opType == 'n') continue;

                const string ns = temp["ns"].String();

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

                OldClientContext ctx(txn, ns);

                Status status(ErrorCodes::InternalError, "");
                while (true) {
                    try {
                        // We assume that in the WriteConflict retry case, either the op rolls back
                        // any changes it makes or is otherwise safe to rerun.
                        status =
                            repl::applyOperation_inlock(txn, ctx.db(), temp, false, alwaysUpsert);
                        break;
                    }
                    catch (const WriteConflictException& wce) {
                        LOG(2) << "WriteConflictException in applyOps command, retrying.";
                        txn->recoveryUnit()->commitAndRestart();
                        continue;
                    }
                }

                ab.append(status.isOK());
                if (!status.isOK()) {
                    errors++;
                }

                num++;

                WriteUnitOfWork wuow(txn);
                logOpForDbHash(txn, ns.c_str());
                wuow.commit();
            }

            result.append( "applied" , num );
            result.append( "results" , ab.arr() );

            if ( ! fromRepl ) {
                // We want this applied atomically on slaves
                // so we re-wrap without the pre-condition for speed

                string tempNS = str::stream() << dbname << ".$cmd";

                // TODO: possibly use mutable BSON to remove preCondition field
                // once it is available
                BSONObjIterator iter(cmdObj);
                BSONObjBuilder cmdBuilder;

                while (iter.more()) {
                    BSONElement elem(iter.next());
                    if (strcmp(elem.fieldName(), "preCondition") != 0) {
                        cmdBuilder.append(elem);
                    }
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
                return false;
            }

            return true;
        }

    private:
        /**
         * Returns true if 'e' contains a valid operation.
         */
        bool _checkOperation(const BSONElement& e, string& errmsg) {
            if (e.type() != Object) {
                errmsg = str::stream() << "op not an object: " << e.fieldName();
                return false;
            }
            BSONObj obj = e.Obj();
            // op - operation type
            BSONElement opElement = obj.getField("op");
            if (opElement.eoo()) {
                errmsg = str::stream() << "op does not contain required \"op\" field: "
                                       << e.fieldName();
                return false;
            }
            if (opElement.type() != mongo::String) {
                errmsg = str::stream() << "\"op\" field is not a string: " << e.fieldName();
                return false;
            }
            // operation type -- see logOp() comments for types
            const char *opType = opElement.valuestrsafe();
            if (*opType == '\0') {
                errmsg = str::stream() << "\"op\" field value cannot be empty: " << e.fieldName();
                return false;
            }

            // ns - namespace
            // Only operations of type 'n' are allowed to have an empty namespace.
            BSONElement nsElement = obj.getField("ns");
            if (nsElement.eoo()) {
                errmsg = str::stream() << "op does not contain required \"ns\" field: "
                                       << e.fieldName();
                return false;
            }
            if (nsElement.type() != mongo::String) {
                errmsg = str::stream() << "\"ns\" field is not a string: " << e.fieldName();
                return false;
            }
            if (*opType != 'n' && nsElement.String().empty()) {
                errmsg = str::stream()
                    << "\"ns\" field value cannot be empty when op type is not 'n': "
                    << e.fieldName();
                return false;
            }
            return true;
        }
    } applyOpsCmd;

}
