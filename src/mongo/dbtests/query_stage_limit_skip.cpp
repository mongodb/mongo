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
 */

/**
 * This file tests db/exec/limit.cpp and db/exec/skip.cpp.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"

using namespace mongo;

namespace {

    static const int N = 50;

    /* Populate a MockStage and return it.  Caller owns it. */
    MockStage* getMS(WorkingSet* ws) {
        auto_ptr<MockStage> ms(new MockStage(ws));

        // Put N ADVANCED results into the mock stage, and some other stalling results (YIELD/TIME).
        for (int i = 0; i < N; ++i) {
            ms->pushBack(PlanStage::NEED_TIME);
            WorkingSetMember wsm;
            wsm.state = WorkingSetMember::OWNED_OBJ;
            wsm.obj = BSON("x" << i);
            ms->pushBack(wsm);
            ms->pushBack(PlanStage::NEED_TIME);
            ms->pushBack(PlanStage::NEED_FETCH);
        }

        return ms.release();
    }

    int countResults(PlanStage* stage) {
        int count = 0;
        while (!stage->isEOF()) {
            WorkingSetID id;
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
    }  queryStageLimitSkipAll;

}  // namespace
