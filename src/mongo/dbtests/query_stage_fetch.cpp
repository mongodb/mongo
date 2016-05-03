/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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
 * This file tests db/exec/fetch.cpp.  Fetch goes to disk so we cannot test outside of a dbtest.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"

namespace QueryStageFetch {

using std::set;
using std::shared_ptr;
using std::unique_ptr;
using stdx::make_unique;

class QueryStageFetchBase {
public:
    QueryStageFetchBase() : _client(&_txn) {}

    virtual ~QueryStageFetchBase() {
        _client.dropCollection(ns());
    }

    void getRecordIds(set<RecordId>* out, Collection* coll) {
        auto cursor = coll->getCursor(&_txn);
        while (auto record = cursor->next()) {
            out->insert(record->id);
        }
    }

    void insert(const BSONObj& obj) {
        _client.insert(ns(), obj);
    }

    void remove(const BSONObj& obj) {
        _client.remove(ns(), obj);
    }

    static const char* ns() {
        return "unittests.QueryStageFetch";
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    DBDirectClient _client;
};


//
// Test that a WSM with an obj is passed through verbatim.
//
class FetchStageAlreadyFetched : public QueryStageFetchBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        WorkingSet ws;

        // Add an object to the DB.
        insert(BSON("foo" << 5));
        set<RecordId> recordIds;
        getRecordIds(&recordIds, coll);
        ASSERT_EQUALS(size_t(1), recordIds.size());

        // Create a mock stage that returns the WSM.
        auto mockStage = make_unique<QueuedDataStage>(&_txn, &ws);

        // Mock data.
        {
            WorkingSetID id = ws.allocate();
            WorkingSetMember* mockMember = ws.get(id);
            mockMember->recordId = *recordIds.begin();
            mockMember->obj = coll->docFor(&_txn, mockMember->recordId);
            ws.transitionToRecordIdAndObj(id);
            // Points into our DB.
            mockStage->pushBack(id);
        }
        {
            WorkingSetID id = ws.allocate();
            WorkingSetMember* mockMember = ws.get(id);
            mockMember->recordId = RecordId();
            mockMember->obj = Snapshotted<BSONObj>(SnapshotId(), BSON("foo" << 6));
            mockMember->transitionToOwnedObj();
            ASSERT_TRUE(mockMember->obj.value().isOwned());
            mockStage->pushBack(id);
        }

        unique_ptr<FetchStage> fetchStage(
            new FetchStage(&_txn, &ws, mockStage.release(), NULL, coll));

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state;

        // Don't bother doing any fetching if an obj exists already.
        state = fetchStage->work(&id);
        ASSERT_EQUALS(PlanStage::ADVANCED, state);
        state = fetchStage->work(&id);
        ASSERT_EQUALS(PlanStage::ADVANCED, state);

        // No more data to fetch, so, EOF.
        state = fetchStage->work(&id);
        ASSERT_EQUALS(PlanStage::IS_EOF, state);
    }
};

//
// Test matching with fetch.
//
class FetchStageFilter : public QueryStageFetchBase {
public:
    void run() {
        ScopedTransaction transaction(&_txn, MODE_IX);
        Lock::DBLock lk(_txn.lockState(), nsToDatabaseSubstring(ns()), MODE_X);
        OldClientContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }

        WorkingSet ws;

        // Add an object to the DB.
        insert(BSON("foo" << 5));
        set<RecordId> recordIds;
        getRecordIds(&recordIds, coll);
        ASSERT_EQUALS(size_t(1), recordIds.size());

        // Create a mock stage that returns the WSM.
        auto mockStage = make_unique<QueuedDataStage>(&_txn, &ws);

        // Mock data.
        {
            WorkingSetID id = ws.allocate();
            WorkingSetMember* mockMember = ws.get(id);
            mockMember->recordId = *recordIds.begin();
            ws.transitionToRecordIdAndIdx(id);

            // State is RecordId and index, shouldn't be able to get the foo data inside.
            BSONElement elt;
            ASSERT_FALSE(mockMember->getFieldDotted("foo", &elt));
            mockStage->pushBack(id);
        }

        // Make the filter.
        BSONObj filterObj = BSON("foo" << 6);
        const CollatorInterface* collator = nullptr;
        StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
            filterObj, ExtensionsCallbackDisallowExtensions(), collator);
        verify(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        // Matcher requires that foo==6 but we only have data with foo==5.
        unique_ptr<FetchStage> fetchStage(
            new FetchStage(&_txn, &ws, mockStage.release(), filterExpr.get(), coll));

        // First call should return a fetch request as it's not in memory.
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state;

        // Normally we'd return the object but we have a filter that prevents it.
        state = fetchStage->work(&id);
        ASSERT_EQUALS(PlanStage::NEED_TIME, state);

        // No more data to fetch, so, EOF.
        state = fetchStage->work(&id);
        ASSERT_EQUALS(PlanStage::IS_EOF, state);
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_fetch") {}

    void setupTests() {
        add<FetchStageAlreadyFetched>();
        add<FetchStageFilter>();
    }
};

SuiteInstance<All> queryStageFetchAll;

}  // namespace QueryStageFetch
