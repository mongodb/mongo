/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

/**
 * This file tests db/exec/index_scan.cpp
 */

namespace mongo {
namespace QueryStageTests {

class IndexScanBase {
public:
    IndexScanBase() : _client(&_opCtx) {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());

        for (int i = 0; i < numObj(); ++i) {
            BSONObjBuilder bob;
            bob.append("foo", i);
            bob.append("baz", i);
            bob.append("bar", numObj() - i);
            _client.insert(nss(), bob.obj());
        }

        addIndex(BSON("foo" << 1));
        addIndex(BSON("foo" << 1 << "baz" << 1));
    }

    virtual ~IndexScanBase() {
        dbtests::WriteContextForTests ctx(&_opCtx, ns());
        _client.dropCollection(nss());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_opCtx, ns(), obj));
    }

    int countResults(const IndexScanParams& params, BSONObj filterObj = BSONObj()) {
        AutoGetCollectionForReadCommand ctx(&_opCtx,
                                            NamespaceString::createNamespaceString_forTest(ns()));

        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterObj, _expCtx);
        MONGO_verify(statusWithMatcher.isOK());
        std::unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        auto ws = std::make_unique<WorkingSet>();
        auto ix = std::make_unique<IndexScan>(
            _expCtx.get(), &ctx.getCollection(), params, ws.get(), filterExpr.get());

        auto statusWithPlanExecutor =
            plan_executor_factory::make(_expCtx,
                                        std::move(ws),
                                        std::move(ix),
                                        &ctx.getCollection(),
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        QueryPlannerParams::DEFAULT);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        auto exec = std::move(statusWithPlanExecutor.getValue());

        int count = 0;
        PlanExecutor::ExecState state;
        for (RecordId dl; PlanExecutor::ADVANCED ==
             (state = exec->getNext(static_cast<BSONObj*>(nullptr), &dl));) {
            ++count;
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);

        return count;
    }

    const IndexDescriptor* getIndex(const BSONObj& obj) {
        AutoGetCollectionForReadCommand collection(
            &_opCtx, NamespaceString::createNamespaceString_forTest(ns()));
        std::vector<const IndexDescriptor*> indexes;
        collection->getIndexCatalog()->findIndexesByKeyPattern(
            &_opCtx, obj, IndexCatalog::InclusionPolicy::kReady, &indexes);
        return indexes.empty() ? nullptr : indexes[0];
    }

    IndexScanParams makeIndexScanParams(OperationContext* opCtx,
                                        const IndexDescriptor* descriptor) {
        AutoGetCollectionForReadCommand collection(
            &_opCtx, NamespaceString::createNamespaceString_forTest(ns()));
        IndexScanParams params(opCtx, *collection, descriptor);
        params.bounds.isSimpleRange = true;
        params.bounds.endKey = BSONObj();
        params.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
        params.direction = 1;
        return params;
    }

    static int numObj() {
        return 50;
    }
    static const char* ns() {
        return "unittests.IndexScan";
    }
    static NamespaceString nss() {
        return NamespaceString::createNamespaceString_forTest(ns());
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;
    boost::intrusive_ptr<ExpressionContext> _expCtx =
        ExpressionContextBuilder{}
            .opCtx(&_opCtx)
            .ns(NamespaceString::createNamespaceString_forTest(ns()))
            .build();

private:
    DBDirectClient _client;
};

class QueryStageIXScanBasic : public IndexScanBase {
public:
    ~QueryStageIXScanBasic() override {}

    void run() {
        // foo <= 20
        auto params = makeIndexScanParams(&_opCtx, this->getIndex(BSON("foo" << 1)));
        params.bounds.startKey = BSON("" << 20);
        params.direction = -1;

        ASSERT_EQUALS(countResults(params), 21);
    }
};

class QueryStageIXScanLowerUpper : public IndexScanBase {
public:
    ~QueryStageIXScanLowerUpper() override {}

    void run() {
        // 20 <= foo < 30
        auto params = makeIndexScanParams(&_opCtx, getIndex(BSON("foo" << 1)));
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);
        params.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;
        params.direction = 1;

        ASSERT_EQUALS(countResults(params), 10);
    }
};

class QueryStageIXScanLowerUpperIncl : public IndexScanBase {
public:
    ~QueryStageIXScanLowerUpperIncl() override {}

    void run() {
        // 20 <= foo <= 30
        auto params = makeIndexScanParams(&_opCtx, getIndex(BSON("foo" << 1)));
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);

        ASSERT_EQUALS(countResults(params), 11);
    }
};

class QueryStageIXScanLowerUpperInclFilter : public IndexScanBase {
public:
    ~QueryStageIXScanLowerUpperInclFilter() override {}

    void run() {
        // 20 <= foo < 30
        // foo == 25
        auto params = makeIndexScanParams(&_opCtx, getIndex(BSON("foo" << 1)));
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);

        ASSERT_EQUALS(countResults(params, BSON("foo" << 25)), 1);
    }
};

class QueryStageIXScanCantMatch : public IndexScanBase {
public:
    ~QueryStageIXScanCantMatch() override {}

    void run() {
        // 20 <= foo < 30
        // bar == 25 (not covered, should error.)
        auto params = makeIndexScanParams(&_opCtx, getIndex(BSON("foo" << 1)));
        params.bounds.startKey = BSON("" << 20);
        params.bounds.endKey = BSON("" << 30);

        ASSERT_THROWS(countResults(params, BSON("baz" << 25)), AssertionException);
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_tests") {}

    void setupTests() override {
        add<QueryStageIXScanBasic>();
        add<QueryStageIXScanLowerUpper>();
        add<QueryStageIXScanLowerUpperIncl>();
        add<QueryStageIXScanLowerUpperInclFilter>();
        add<QueryStageIXScanCantMatch>();
    }
};

unittest::OldStyleSuiteInitializer<All> queryStageTestsAll;

}  // namespace QueryStageTests
}  // namespace mongo
