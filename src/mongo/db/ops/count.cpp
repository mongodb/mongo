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
#include "mongo/db/pdfile.h"
#include "mongo/db/query/get_runner.h"

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

    long long runCount( const char *ns, const BSONObj &cmd, string &err, int &errCode ) {
        // Lock 'ns'.
        Client::Context cx(ns);

        NamespaceDetails *d = nsdetails(ns);
        if (NULL == d) {
            err = "ns missing";
            return -1;
        }

        BSONObj query = cmd.getObjectField("query");
        long long count = 0;
        long long skip = cmd["skip"].numberLong();
        long long limit = cmd["limit"].numberLong();

        if (limit < 0) {
            limit = -limit;
        }
        
        // count of all objects
        if (query.isEmpty()) {
            return applySkipLimit(d->numRecords(), cmd);
        }

        CanonicalQuery* cq;
        // We pass -limit because a positive limit means 'batch size' but negative limit is a
        // hard limit.
        if (!CanonicalQuery::canonicalize(ns, query, skip, -limit, &cq).isOK()) {
            uasserted(17220, "could not canonicalize query " + query.toString());
            return -2;
        }

        Runner* rawRunner;
        if (!getRunner(cq, &rawRunner).isOK()) {
            uasserted(17221, "could not get runner " + query.toString());
            return -2;
        }

        auto_ptr<Runner> runner(rawRunner);
        auto_ptr<DeregisterEvenIfUnderlyingCodeThrows> safety;

        try {
            ClientCursor::registerRunner(runner.get());
            runner->setYieldPolicy(Runner::YIELD_AUTO);
            safety.reset(new DeregisterEvenIfUnderlyingCodeThrows(runner.get()));

            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, NULL))) {
                ++count;
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
