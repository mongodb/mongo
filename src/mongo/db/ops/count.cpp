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

    long long runCount( const string& ns, const BSONObj &cmd, string &err, int &errCode ) {
        // Lock 'ns'.
        Client::Context cx(ns);
        Collection* collection = cx.db()->getCollection(ns);

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

        Runner* rawRunner;
        long long skip = cmd["skip"].numberLong();
        long long limit = cmd["limit"].numberLong();

        if (limit < 0) {
            limit = -limit;
        }

        uassertStatusOK(getRunnerCount(collection, query, hintObj, &rawRunner));
        auto_ptr<Runner> runner(rawRunner);

        try {
            const ScopedRunnerRegistration safety(runner.get());
            runner->setYieldPolicy(Runner::YIELD_AUTO);

            long long count = 0;
            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, NULL))) {
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
    
} // namespace mongo
