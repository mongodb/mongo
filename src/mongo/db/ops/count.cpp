// count.cpp

/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/ops/count.h"

#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/curop.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/type_explain.h"

namespace {

    using namespace mongo;

    /**
     * Ask 'runner' for a summary of the plan it is using to run the count command,
     * and store this information in 'currentOp'.
     *
     * Returns true if the planSummary was copied to 'currentOp' and false otherwise.
     */
    bool setPlanSummary(Runner* runner, CurOp* currentOp) {
        if (NULL != currentOp) {
            PlanInfo* rawInfo;
            Status s = runner->getInfo(NULL, &rawInfo);
            if (s.isOK()) {
                scoped_ptr<PlanInfo> planInfo(rawInfo);
                currentOp->debug().planSummary = planInfo->planSummary.c_str();
                return true;
            }
        }

        return false;
    }

} // namespace

namespace mongo {

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

    long long runCount( const string& ns, const BSONObj &cmd, string &err, int &errCode ) {
        // Lock 'ns'.
        Client::Context cx(ns);
        Collection* collection = cx.db()->getCollection(ns);

        if (NULL == collection) {
            err = "ns missing";
            return -1;
        }

        BSONObj query = cmd.getObjectField("query");

        BSONObj hintObj;
        if (Object == cmd["hint"].type()) {
            hintObj = cmd["hint"].Obj();
        }
        else if (String == cmd["hint"].type()) {
            const std::string hint = cmd.getStringField("hint");
            hintObj = BSON("$hint" << hint);
        }

        // count of all objects
        if (query.isEmpty()) {
            return applySkipLimit(collection->numRecords(), cmd);
        }

        Runner* rawRunner;
        long long skip = cmd["skip"].numberLong();
        long long limit = cmd["limit"].numberLong();

        if (limit < 0) {
            limit = -limit;
        }

        uassertStatusOK(getRunnerCount(collection, query, hintObj, &rawRunner));
        auto_ptr<Runner> runner(rawRunner);

        // Get a pointer to the current operation. We will try to copy the planSummary
        // there so that it appears in db.currentOp() and the slow query log.
        Client& client = cc();
        CurOp* currentOp = client.curop();

        // Have we copied the planSummary to 'currentOp' yet?
        bool gotPlanSummary = false;

        // Try to copy the plan summary to the 'currentOp'.
        if (!gotPlanSummary) {
            gotPlanSummary = setPlanSummary(runner.get(), currentOp);
        }

        try {
            const ScopedRunnerRegistration safety(runner.get());
            runner->setYieldPolicy(Runner::YIELD_AUTO);

            long long count = 0;
            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, NULL))) {
                // Try to copy the plan summary to the 'currentOp'. We need to try again
                // here because we might not have chosen a plan until after the first
                // call to getNext(...).
                if (!gotPlanSummary) {
                    gotPlanSummary = setPlanSummary(runner.get(), currentOp);
                }

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

            // Try to copy the plan summary to the 'currentOp', if we haven't already. This
            // could happen if, for example, the count is 0.
            if (!gotPlanSummary) {
                gotPlanSummary = setPlanSummary(runner.get(), currentOp);
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
    
} // namespace mongo
