/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/count.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/util/log.h"

namespace mongo {

    static CmdCount cmdCount;

    static long long applySkipLimit(long long num, const BSONObj& cmd) {
        BSONElement s = cmd["skip"];
        BSONElement l = cmd["limit"];

        if (s.isNumber()) {
            num = num - s.numberLong();
            if (num < 0) {
                num = 0;
            }
        }

        if (l.isNumber()) {
            long long limit = l.numberLong();
            if (limit < 0) {
                limit = -limit;
            }

            // 0 means no limit.
            if (limit < num && limit != 0) {
                num = limit;
            }
        }

        return num;
    }

    long long runCount(OperationContext* txn,
                       const string& ns,
                       const BSONObj &cmd,
                       string &err,
                       int &errCode) {
        // Lock 'ns'.
        Client::Context cx(txn, ns);
        Collection* collection = cx.db()->getCollection(txn, ns);
        const string& dbname = cx.db()->name();

        if (NULL == collection) {
            err = "ns missing";
            return -1;
        }

        BSONObj query = cmd.getObjectField("query");
        const std::string hint = cmd.getStringField("hint");
        const BSONObj hintObj = hint.empty() ? BSONObj() : BSON("$hint" << hint);

        // count of all objects
        if (query.isEmpty()) {
            return applySkipLimit(collection->numRecords(), cmd);
        }

        long long skip = cmd["skip"].numberLong();
        long long limit = cmd["limit"].numberLong();

        if (limit < 0) {
            limit = -limit;
        }

        // Get an executor for the command.
        PlanExecutor* rawExec;
        uassertStatusOK(CmdCount::parseCountToExecutor(txn, cmd, dbname, ns, collection, &rawExec));
        scoped_ptr<PlanExecutor> exec(rawExec);

        // Store the plan summary string in CurOp.
        Client& client = cc();
        CurOp* currentOp = client.curop();
        if (NULL != currentOp) {
            PlanSummaryStats stats;
            Explain::getSummaryStats(exec.get(), &stats);
            currentOp->debug().planSummary = stats.summaryStr.c_str();
        }

        try {
            ScopedExecutorRegistration safety(exec.get());

            long long count = 0;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(NULL, NULL))) {
                if (skip > 0) {
                    --skip;
                }
                else {
                    ++count;
                    // Fast-path. There's no point in iterating all over the runner if limit
                    // is set.
                    if (count >= limit && limit != 0) {
                        break;
                    }
                }
            }

            // Emulate old behavior and return the count even if the runner was killed.  This
            // happens when the underlying collection is dropped.
            return count;
        }
        catch (const DBException &e) {
            err = e.toString();
            errCode = e.getCode();
        }
        catch (const std::exception &e) {
            err = e.what();
            errCode = 0;
        }

        // Historically we have returned zero in many count assertion cases - see SERVER-2291.
        log() << "Count with ns: " << ns << " and query: " << query
              << " failed with exception: " << err << " code: " << errCode
              << endl;

        return -2;
    }

    Status CmdCount::explain(OperationContext* txn,
                             const std::string& dbname,
                             const BSONObj& cmdObj,
                             Explain::Verbosity verbosity,
                             BSONObjBuilder* out) const {

        // Acquire the DB read lock and get the collection.
        const string ns = parseNs(dbname, cmdObj);
        Client::ReadContext ctx(txn, ns);
        Collection* collection = ctx.ctx().db()->getCollection( txn, ns );

        // Handle special case of an empty query. When there is no query, we don't construct
        // an actual execution tree. Since each collection tracks the number of documents it
        // contains, count ops with no query just ask the collection for the total number of
        // documents (and then apply the skip/limit to this number if necessary).
        BSONObj query = cmdObj.getObjectField("query");
        if (query.isEmpty()) {
            Explain::explainCountEmptyQuery(out);
            return Status::OK();
        }

        // Get an executor for the command and use it to generate the explain output.
        PlanExecutor* rawExec;
        Status execStatus = parseCountToExecutor(txn, cmdObj, dbname, ns, collection, &rawExec);
        if (!execStatus.isOK()) {
            return execStatus;
        }

        scoped_ptr<PlanExecutor> exec(rawExec);

        return Explain::explainStages(exec.get(), verbosity, out);
    }

    // static
    Status CmdCount::parseCountToExecutor(OperationContext* txn,
                                          const BSONObj& cmdObj,
                                          const std::string& dbname,
                                          const std::string& ns,
                                          Collection* collection,
                                          PlanExecutor** execOut) {

        long long skip = 0;
        if (cmdObj["skip"].isNumber()) {
            skip = cmdObj["skip"].numberLong();
            if (skip < 0) {
                return Status(ErrorCodes::BadValue, "skip value is negative in count query");
            }
        }
        else if (cmdObj["skip"].ok()) {
            return Status(ErrorCodes::BadValue, "skip value is not a valid number");
        }

        BSONObj query = cmdObj.getObjectField("query");
        invariant(!query.isEmpty());

        const std::string hint = cmdObj.getStringField("hint");
        const BSONObj hintObj = hint.empty() ? BSONObj() : BSON("$hint" << hint);

        StringData dbnameData(dbname);
        const WhereCallbackReal whereCallback(txn, dbnameData);

        CanonicalQuery* cq;
        uassertStatusOK(CanonicalQuery::canonicalize(ns,
                                                     query,
                                                     BSONObj(),
                                                     BSONObj(),
                                                     0,
                                                     0,
                                                     hintObj,
                                                     &cq,
                                                     whereCallback));

        // Takes ownership of 'cq'.
        return getExecutor(txn, collection, cq, execOut, QueryPlannerParams::PRIVATE_IS_COUNT);
    }

    bool CmdCount::run(OperationContext* txn,
                       const string& dbname,
                       BSONObj& cmdObj,
                       int, string& errmsg,
                       BSONObjBuilder& result, bool) {

        long long skip = 0;
        if ( cmdObj["skip"].isNumber() ) {
            skip = cmdObj["skip"].numberLong();
            if ( skip < 0 ) {
                errmsg = "skip value is negative in count query";
                return false;
            }
        }
        else if ( cmdObj["skip"].ok() ) {
            errmsg = "skip value is not a valid number";
            return false;
        }

        const string ns = parseNs(dbname, cmdObj);

        // This acquires the DB read lock
        //
        Client::ReadContext ctx(txn, ns);

        string err;
        int errCode;
        long long n = runCount(txn, ns, cmdObj, err, errCode);

        long long retVal = n;
        bool ok = true;
        if ( n == -1 ) {
            retVal = 0;
            result.appendBool( "missing" , true );
        }
        else if ( n < 0 ) {
            retVal = 0;
            ok = false;
            if ( !err.empty() ) {
                errmsg = err;
                result.append("code", errCode);
                return false;
            }
        }

        result.append("n", static_cast<double>(retVal));
        return ok;
    }

} // namespace mongo
