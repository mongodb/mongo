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
 * This file tests db/exec/fetch.cpp.  Fetch goes to disk so we cannot test outside of a dbtest.
 */

#include <boost/shared_ptr.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/cursor.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pdfile.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_registry.h"
#include "mongo/util/fail_point_service.h"

namespace QueryStageFetch {

    class QueryStageFetchBase {
    public:
        QueryStageFetchBase() { }

        virtual ~QueryStageFetchBase() {
            _client.dropCollection(ns());
        }

        void getLocs(set<DiskLoc>* out) {
            for (boost::shared_ptr<Cursor> c = theDataFileMgr.findAll(ns()); c->ok(); c->advance()) {
                out->insert(c->currLoc());
            }
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        static const char* ns() { return "unittests.QueryStageFetch"; }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient QueryStageFetchBase::_client;

    //
    // Test that a fetch is passed up when it's not in memory.
    //
    class FetchStageNotInMemory : public QueryStageFetchBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());
            WorkingSet ws;

            // Add an object to the DB.
            insert(BSON("foo" << 5));
            set<DiskLoc> locs;
            getLocs(&locs);
            ASSERT_EQUALS(size_t(1), locs.size());

            // Create a mock stage that returns the WSM.
            auto_ptr<MockStage> mockStage(new MockStage(&ws));

            // Mock data.
            {
                WorkingSetMember mockMember;
                mockMember.state = WorkingSetMember::LOC_AND_IDX;
                mockMember.loc = *locs.begin();

                // State is loc and index, shouldn't be able to get the foo data inside.
                BSONElement elt;
                ASSERT_FALSE(mockMember.getFieldDotted("foo", &elt));
                mockStage->pushBack(mockMember);
            }

            auto_ptr<FetchStage> fetchStage(new FetchStage(&ws, mockStage.release(), NULL));

            // Set the fail point to return not in memory.
            FailPointRegistry* reg = getGlobalFailPointRegistry();
            FailPoint* fetchInMemoryFail = reg->getFailPoint("fetchInMemoryFail");
            fetchInMemoryFail->setMode(FailPoint::alwaysOn);

            // First call should return a fetch request as it's not in memory.
            WorkingSetID id;
            PlanStage::StageState state;
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::NEED_FETCH, state);

            // Let's do the fetch ourselves (though it doesn't really matter)
            WorkingSetMember* member = ws.get(id);
            ASSERT_FALSE(member->hasObj());
            member->loc.rec()->touch();

            // Next call to work() should give us the object in a diff. state
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::ADVANCED, state);
            ASSERT_EQUALS(WorkingSetMember::LOC_AND_UNOWNED_OBJ, member->state);

            // We should be able to get data from the obj now.
            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(elt.numberInt(), 5);

            // Mock stage is EOF so fetch should be too.
            ASSERT_TRUE(fetchStage->isEOF());

            // Turn off fail point for further tests.
            fetchInMemoryFail->setMode(FailPoint::off);
        }
    };

    //
    // Test that a fetch is not passed up when it's in memory.
    //
    class FetchStageInMemory : public QueryStageFetchBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());
            WorkingSet ws;

            // Add an object to the DB.
            insert(BSON("foo" << 5));
            set<DiskLoc> locs;
            getLocs(&locs);
            ASSERT_EQUALS(size_t(1), locs.size());

            // Create a mock stage that returns the WSM.
            auto_ptr<MockStage> mockStage(new MockStage(&ws));

            // Mock data.
            {
                WorkingSetMember mockMember;
                mockMember.state = WorkingSetMember::LOC_AND_IDX;
                mockMember.loc = *locs.begin();

                // State is loc and index, shouldn't be able to get the foo data inside.
                BSONElement elt;
                ASSERT_FALSE(mockMember.getFieldDotted("foo", &elt));
                mockStage->pushBack(mockMember);
            }

            auto_ptr<FetchStage> fetchStage(new FetchStage(&ws, mockStage.release(), NULL));

            // Set the fail point to return in memory.
            FailPointRegistry* reg = getGlobalFailPointRegistry();
            FailPoint* fetchInMemorySucceed = reg->getFailPoint("fetchInMemorySucceed");
            fetchInMemorySucceed->setMode(FailPoint::alwaysOn);

            // First call fetches as expected.
            WorkingSetID id;
            PlanStage::StageState state;
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::ADVANCED, state);

            // State should have changed.
            WorkingSetMember* member = ws.get(id);
            ASSERT_EQUALS(WorkingSetMember::LOC_AND_UNOWNED_OBJ, member->state);

            // We should be able to get data from the obj now.
            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(elt.numberInt(), 5);

            // Mock stage is EOF so fetch should be too.
            ASSERT_TRUE(fetchStage->isEOF());

            // Turn off fail point for further tests.
            fetchInMemorySucceed->setMode(FailPoint::off);
        }
    };

    //
    // Test mid-fetch invalidation.
    //
    class FetchStageInvalidation : public QueryStageFetchBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());
            WorkingSet ws;

            // Add an object to the DB.
            insert(BSON("foo" << 5));
            set<DiskLoc> locs;
            getLocs(&locs);
            ASSERT_EQUALS(size_t(1), locs.size());

            // Create a mock stage that returns the WSM.
            auto_ptr<MockStage> mockStage(new MockStage(&ws));

            // Mock data.
            {
                WorkingSetMember mockMember;
                mockMember.state = WorkingSetMember::LOC_AND_IDX;
                mockMember.loc = *locs.begin();

                // State is loc and index, shouldn't be able to get the foo data inside.
                BSONElement elt;
                ASSERT_FALSE(mockMember.getFieldDotted("foo", &elt));
                mockStage->pushBack(mockMember);
            }

            auto_ptr<FetchStage> fetchStage(new FetchStage(&ws, mockStage.release(), NULL));

            // Set the fail point to return not in memory.
            FailPointRegistry* reg = getGlobalFailPointRegistry();
            FailPoint* fetchInMemoryFail = reg->getFailPoint("fetchInMemoryFail");
            fetchInMemoryFail->setMode(FailPoint::alwaysOn);

            // First call should return a fetch request as it's not in memory.
            WorkingSetID id;
            PlanStage::StageState state;
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::NEED_FETCH, state);

            WorkingSetMember* member = ws.get(id);

            // Invalidate the DL.
            fetchStage->invalidate(member->loc);

            // Next call to work() should give us the OWNED obj as it was invalidated mid-page-in.
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::ADVANCED, state);
            ASSERT_EQUALS(WorkingSetMember::OWNED_OBJ, member->state);

            // We should be able to get data from the obj now.
            BSONElement elt;
            ASSERT_TRUE(member->getFieldDotted("foo", &elt));
            ASSERT_EQUALS(elt.numberInt(), 5);

            // Mock stage is EOF so fetch should be too.
            ASSERT_TRUE(fetchStage->isEOF());

            // Turn off fail point for further tests.
            fetchInMemoryFail->setMode(FailPoint::off);
        }
    };


    //
    // Test that a WSM with an obj is passed through verbatim.
    //
    class FetchStageAlreadyFetched : public QueryStageFetchBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());
            WorkingSet ws;

            // Add an object to the DB.
            insert(BSON("foo" << 5));
            set<DiskLoc> locs;
            getLocs(&locs);
            ASSERT_EQUALS(size_t(1), locs.size());

            // Create a mock stage that returns the WSM.
            auto_ptr<MockStage> mockStage(new MockStage(&ws));

            // Mock data.
            {
                WorkingSetMember mockMember;
                mockMember.state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
                mockMember.loc = *locs.begin();
                mockMember.obj = mockMember.loc.obj();
                // Points into our DB.
                ASSERT_FALSE(mockMember.obj.isOwned());
                mockStage->pushBack(mockMember);

                mockMember.state = WorkingSetMember::OWNED_OBJ;
                mockMember.loc = DiskLoc();
                mockMember.obj = BSON("foo" << 6);
                ASSERT_TRUE(mockMember.obj.isOwned());
                mockStage->pushBack(mockMember);
            }

            auto_ptr<FetchStage> fetchStage(new FetchStage(&ws, mockStage.release(), NULL));

            // Set the fail point to return not in memory so we get a fetch request.
            FailPointRegistry* reg = getGlobalFailPointRegistry();
            FailPoint* fetchInMemoryFail = reg->getFailPoint("fetchInMemoryFail");
            fetchInMemoryFail->setMode(FailPoint::alwaysOn);

            WorkingSetID id;
            PlanStage::StageState state;

            // Don't bother doing any fetching if an obj exists already.
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::ADVANCED, state);
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::ADVANCED, state);

            // No more data to fetch, so, EOF.
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::IS_EOF, state);

            fetchInMemoryFail->setMode(FailPoint::off);
        }
    };

    //
    // Test matching with fetch.
    //
    class FetchStageFilter : public QueryStageFetchBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());
            WorkingSet ws;

            // Add an object to the DB.
            insert(BSON("foo" << 5));
            set<DiskLoc> locs;
            getLocs(&locs);
            ASSERT_EQUALS(size_t(1), locs.size());

            // Create a mock stage that returns the WSM.
            auto_ptr<MockStage> mockStage(new MockStage(&ws));

            // Mock data.
            {
                WorkingSetMember mockMember;
                mockMember.state = WorkingSetMember::LOC_AND_IDX;
                mockMember.loc = *locs.begin();

                // State is loc and index, shouldn't be able to get the foo data inside.
                BSONElement elt;
                ASSERT_FALSE(mockMember.getFieldDotted("foo", &elt));
                mockStage->pushBack(mockMember);
            }

            // Make the filter.
            BSONObj filterObj = BSON("foo" << 6);
            StatusWithMatchExpression swme = MatchExpressionParser::parse(filterObj);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filterExpr(swme.getValue());

            // Matcher requires that foo==6 but we only have data with foo==5.
            auto_ptr<FetchStage> fetchStage(new FetchStage(&ws, mockStage.release(),
                                                           filterExpr.get()));

            // Set the fail point to return not in memory so we get a fetch request.
            FailPointRegistry* reg = getGlobalFailPointRegistry();
            FailPoint* fetchInMemoryFail = reg->getFailPoint("fetchInMemoryFail");
            fetchInMemoryFail->setMode(FailPoint::alwaysOn);

            // First call should return a fetch request as it's not in memory.
            WorkingSetID id;
            PlanStage::StageState state;
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::NEED_FETCH, state);

            // Normally we'd return the object but we have a filter that prevents it.
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::NEED_TIME, state);

            // No more data to fetch, so, EOF.
            state = fetchStage->work(&id);
            ASSERT_EQUALS(PlanStage::IS_EOF, state);

            fetchInMemoryFail->setMode(FailPoint::off);
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_fetch" ) { }

        void setupTests() {
            add<FetchStageNotInMemory>();
            add<FetchStageInMemory>();
            add<FetchStageAlreadyFetched>();
            add<FetchStageInvalidation>();
            add<FetchStageFilter>();
        }
    }  queryStageFetchAll;

}  // namespace QueryStageFetch
