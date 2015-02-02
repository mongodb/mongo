/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * This file tests db/exec/limit.cpp and db/exec/skip.cpp.
 */

#include <boost/scoped_ptr.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"

using namespace mongo;

namespace {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::max;
    using std::min;

    static const int N = 50;

    /* Populate a QueuedDataStage and return it.  Caller owns it. */
    QueuedDataStage* getMS(WorkingSet* ws) {
        auto_ptr<QueuedDataStage> ms(new QueuedDataStage(ws));

        // Put N ADVANCED results into the mock stage, and some other stalling results (YIELD/TIME).
        for (int i = 0; i < N; ++i) {
            ms->pushBack(PlanStage::NEED_TIME);
            WorkingSetMember wsm;
            wsm.state = WorkingSetMember::OWNED_OBJ;
            wsm.obj = Snapshotted<BSONObj>(SnapshotId(), BSON("x" << i));
            ms->pushBack(wsm);
            ms->pushBack(PlanStage::NEED_TIME);
        }

        return ms.release();
    }

    int countResults(PlanStage* stage) {
        int count = 0;
        while (!stage->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState status = stage->work(&id);
            if (PlanStage::ADVANCED != status) { continue; }
            ++count;
        }
        return count;
    }

    //
    // Insert 50 objects.  Filter/skip 0, 1, 2, ..., 100 objects and expect the right # of results.
    //
    class QueryStageLimitSkipBasicTest {
    public:
        void run() {
            for (int i = 0; i < 2 * N; ++i) {
                WorkingSet ws;

                scoped_ptr<PlanStage> skip(new SkipStage(i, &ws, getMS(&ws)));
                ASSERT_EQUALS(max(0, N - i), countResults(skip.get()));

                scoped_ptr<PlanStage> limit(new LimitStage(i, &ws, getMS(&ws)));
                ASSERT_EQUALS(min(N, i), countResults(limit.get()));
            }
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_limit_skip" ) { }

        void setupTests() {
            add<QueryStageLimitSkipBasicTest>();
        }
    };

    SuiteInstance<All> queryStageLimitSkipAll;

}  // namespace
