/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/classic_stage_builder.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

const static NamespaceString kNss("db.dummy");

class ClassicStageBuilderTest : public ServiceContextMongoDTest {
public:
    void setUp() {
        getServiceContext()->setFastClockSource(std::make_unique<ClockSourceMock>());
        _opCtx = makeOperationContext();
        _workingSet = std::make_unique<WorkingSet>();
    }

    void tearDown() {
        _opCtx.reset();
        _workingSet.reset();
    }

    /**
     * Converts a 'QuerySolutionNode' to a 'QuerySolution'.
     */
    std::unique_ptr<QuerySolution> makeQuerySolution(std::unique_ptr<QuerySolutionNode> root) {
        auto querySoln = std::make_unique<QuerySolution>(QueryPlannerParams::Options::DEFAULT);
        querySoln->setRoot(std::move(root));
        return querySoln;
    }

    /**
     * Builds a PlanStage using the given WorkingSet and QuerySolution.
     */
    std::unique_ptr<PlanStage> buildPlanStage(std::unique_ptr<QuerySolution> querySolution) {
        auto qr = std::make_unique<QueryRequest>(kNss);
        auto expCtx = make_intrusive<ExpressionContext>(opCtx(), nullptr, kNss);
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(qr), expCtx);
        ASSERT_OK(statusWithCQ.getStatus());

        stage_builder::ClassicStageBuilder builder{
            opCtx(), CollectionPtr::null, *statusWithCQ.getValue(), *querySolution, workingSet()};
        return builder.build(querySolution->root());
    }

    /**
     * A helper to repeatedly call work() until the stage returns a PlanStage::IS_EOF state and
     * returns the resulting documents as a vector of BSONObj.
     */
    std::vector<BSONObj> collectResults(std::unique_ptr<PlanStage> stage) {
        WorkingSetID id;
        std::vector<BSONObj> results;
        auto state = PlanStage::ADVANCED;

        while (state != PlanStage::IS_EOF) {
            state = stage->work(&id);
            if (state == PlanStage::ADVANCED) {
                auto member = workingSet()->get(id);
                auto doc = member->doc.value().toBson();
                results.push_back(doc);
            }
        }
        return results;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    WorkingSet* workingSet() {
        return _workingSet.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<WorkingSet> _workingSet;
};


/**
 * Verify that a VirtualScanNode can be translated to a QueuedDataStage and produce a data stream.
 */
TEST_F(ClassicStageBuilderTest, VirtualScanTranslation) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 2)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 2))};

    // Construct a QuerySolution consisting of a single VirtualScanNode to test if a stream of
    // documents can be produced.
    auto virtScan =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    // Make a QuerySolution from the root virtual scan node.
    auto querySolution = makeQuerySolution(std::move(virtScan));
    ASSERT_EQ(querySolution->root()->nodeId(), 1);

    // Translate the QuerySolution to a classic PlanStage.
    auto stage = buildPlanStage(std::move(querySolution));

    // Work the stage and collect the results.
    auto results = collectResults(std::move(stage));
    ASSERT_EQ(results.size(), 3);

    // Check that the results produced from the translated VirtualScanNode meet expectation.
    for (size_t i = 0; i < docs.size(); ++i) {
        BSONObjIterator arrIt{docs[i]};
        auto firstElt = arrIt.next();
        ASSERT_BSONOBJ_EQ(firstElt.embeddedObject(), results[i]);
    }
}
}  // namespace mongo
