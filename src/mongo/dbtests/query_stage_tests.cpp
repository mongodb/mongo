// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/index_scan.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/index_builds/index_build_test_helpers.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
        _client.dropCollection(nss());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(createIndex(&_opCtx, ns(), obj));
    }

    int countResults(const IndexScanParams& params, BSONObj filterObj = BSONObj()) {
        const auto collection = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest(NamespaceString::createNamespaceString_forTest(ns()),
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(&_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);

        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterObj, _expCtx);
        MONGO_verify(statusWithMatcher.isOK());
        std::unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        auto ws = std::make_unique<WorkingSet>();
        auto ix = std::make_unique<IndexScan>(
            _expCtx.get(), collection, params, ws.get(), filterExpr.get());

        auto exec = plan_executor_factory::make(_expCtx,
                                                std::move(ws),
                                                std::move(ix),
                                                collection,
                                                PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                QueryPlannerParams::DEFAULT);

        int count = 0;
        PlanExecutor::ExecState state;
        for (RecordId dl; PlanExecutor::ADVANCED ==
             (state = exec->getNext(static_cast<BSONObj*>(nullptr), &dl));) {
            ++count;
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);

        return count;
    }

    const IndexCatalogEntry* getIndex(const BSONObj& obj) {
        const auto collection = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest(NamespaceString::createNamespaceString_forTest(ns()),
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(&_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        std::vector<const IndexCatalogEntry*> indexes;
        collection.getCollectionPtr()->getIndexCatalog()->findIndexesByKeyPattern(
            &_opCtx, obj, IndexCatalog::InclusionPolicy::kReady, &indexes);
        return indexes.empty() ? nullptr : indexes[0];
    }

    IndexScanParams makeIndexScanParams(OperationContext* opCtx, const IndexCatalogEntry* entry) {
        const auto collection = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest(NamespaceString::createNamespaceString_forTest(ns()),
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(&_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        IndexScanParams params(opCtx, collection.getCollectionPtr(), entry);
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
