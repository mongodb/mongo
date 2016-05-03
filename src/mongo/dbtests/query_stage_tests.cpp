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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"

/**
 * This file tests db/exec/index_scan.cpp
 */

namespace QueryStageTests {

using std::unique_ptr;

class IndexScanBase {
public:
    IndexScanBase() : _client(&_txn) {
        OldClientWriteContext ctx(&_txn, ns());

        for (int i = 0; i < numObj(); ++i) {
            BSONObjBuilder bob;
            bob.append("foo", i);
            bob.append("baz", i);
            bob.append("bar", numObj() - i);
            _client.insert(ns(), bob.obj());
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("foo" << 1 << "baz" << 1));
    }

    virtual ~IndexScanBase() {
        OldClientWriteContext ctx(&_txn, ns());
        _client.dropCollection(ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, ns(), obj));
    }

    int countResults(const IndexScanParams& params, BSONObj filterObj = BSONObj()) {
        AutoGetCollectionForRead ctx(&_txn, ns());

        const CollatorInterface* collator = nullptr;
        StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
            filterObj, ExtensionsCallbackDisallowExtensions(), collator);
        verify(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        unique_ptr<WorkingSet> ws = stdx::make_unique<WorkingSet>();
        unique_ptr<IndexScan> ix =
            stdx::make_unique<IndexScan>(&_txn, params, ws.get(), filterExpr.get());

        auto statusWithPlanExecutor = PlanExecutor::make(
            &_txn, std::move(ws), std::move(ix), ctx.getCollection(), PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        int count = 0;
        PlanExecutor::ExecState state;
        for (RecordId dl; PlanExecutor::ADVANCED == (state = exec->getNext(NULL, &dl));) {
            ++count;
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);

        return count;
    }

    void makeGeoData() {
        OldClientWriteContext ctx(&_txn, ns());

        for (int i = 0; i < numObj(); ++i) {
            double lat = double(rand()) / RAND_MAX;
            double lng = double(rand()) / RAND_MAX;
            _client.insert(ns(), BSON("geo" << BSON_ARRAY(lng << lat)));
        }
    }

    IndexDescriptor* getIndex(const BSONObj& obj) {
        AutoGetCollectionForRead ctx(&_txn, ns());
        Collection* collection = ctx.getCollection();
        return collection->getIndexCatalog()->findIndexByKeyPattern(&_txn, obj);
    }

    static int numObj() {
        return 50;
    }
    static const char* ns() {
        return "unittests.IndexScan";
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;

private:
    DBDirectClient _client;
};

class QueryStageIXScanBasic : public IndexScanBase {
public:
    virtual ~QueryStageIXScanBasic() {}

    void run() {
        // foo <= 20
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1));
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSONObj();
        params.bounds.endKeyInclusive = true;
        params.direction = -1;

        ASSERT_EQUALS(countResults(params), 21);
    }
};

class QueryStageIXScanLowerUpper : public IndexScanBase {
public:
    virtual ~QueryStageIXScanLowerUpper() {}

    void run() {
        // 20 <= foo < 30
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1));
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);
        params.bounds.endKeyInclusive = false;
        params.direction = 1;

        ASSERT_EQUALS(countResults(params), 10);
    }
};

class QueryStageIXScanLowerUpperIncl : public IndexScanBase {
public:
    virtual ~QueryStageIXScanLowerUpperIncl() {}

    void run() {
        // 20 <= foo <= 30
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1));
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;

        ASSERT_EQUALS(countResults(params), 11);
    }
};

class QueryStageIXScanLowerUpperInclFilter : public IndexScanBase {
public:
    virtual ~QueryStageIXScanLowerUpperInclFilter() {}

    void run() {
        // 20 <= foo < 30
        // foo == 25
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1));
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;

        ASSERT_EQUALS(countResults(params, BSON("foo" << 25)), 1);
    }
};

class QueryStageIXScanCantMatch : public IndexScanBase {
public:
    virtual ~QueryStageIXScanCantMatch() {}

    void run() {
        // 20 <= foo < 30
        // bar == 25 (not covered, should error.)
        IndexScanParams params;
        params.descriptor = getIndex(BSON("foo" << 1));
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);
        params.bounds.endKeyInclusive = true;
        params.direction = 1;

        ASSERT_THROWS(countResults(params, BSON("baz" << 25)), MsgAssertionException);
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_tests") {}

    void setupTests() {
        add<QueryStageIXScanBasic>();
        add<QueryStageIXScanLowerUpper>();
        add<QueryStageIXScanLowerUpperIncl>();
        add<QueryStageIXScanLowerUpperInclFilter>();
        add<QueryStageIXScanCantMatch>();
    }
};

SuiteInstance<All> queryStageTestsAll;

}  // namespace
